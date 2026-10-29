// inject.js — runs in the MAIN world (the page's own JS context), so it can see
// keybr's real network calls and IndexedDB writes. A content script in the
// isolated world CANNOT (its window.fetch/XHR are separate objects) — which is
// why the original fetch hook never fired and live data never worked.
//
// Strategy: capture anything result-shaped that keybr produces, regardless of
// whether you're signed in (network) or local-only (IndexedDB), and hand it to
// the isolated content script via window.postMessage. The native host dedups by
// timeStamp, so capturing the same result twice is harmless.
//
// 2026-07-04 — the signed-in per-lesson submission was never caught: the old
// build only hooked XHR + fetch + IndexedDB, but keybr can save via
// navigator.sendBeacon or a WebSocket (neither was hooked) and can send bodies
// that aren't plain strings. This version hooks sendBeacon + WebSocket, harvests
// Blob/ArrayBuffer/FormData/URLSearchParams bodies too, and — as a safety net —
// logs every outgoing submission-shaped call to the native host (/tmp/keybr-host
// .log) so one real lesson reveals the exact call if capture still misses.

(function () {
    "use strict";

    // A keybr "result" in export shape: numeric speed, ISO timeStamp string, and
    // a histogram array. STRICT match = what we forward to the graph (keybr-dash
    // needs the histogram). We search shallowly because the app sometimes wraps
    // it ({ result: {...} }, arrays, etc.).
    function findResult(o, depth) {
        if (!o || typeof o !== "object" || depth > 5) return null;
        if (typeof o.speed === "number" && typeof o.timeStamp === "string"
            && Array.isArray(o.histogram)) return o;
        for (const k in o) {
            let v;
            try { v = o[k]; } catch (_) { continue; }   // skip throwing getters
            const r = findResult(v, depth + 1);
            if (r) return r;
        }
        return null;
    }

    // LOOSE match = speed + timeStamp with NO histogram requirement. We never
    // forward these (could corrupt the graph); we only use them to DIAGNOSE — if
    // a submission carries a result-ish object we don't strictly recognise, the
    // diag log tells us the shape so we can widen findResult precisely.
    function looksResultish(o, depth) {
        if (!o || typeof o !== "object" || depth > 5) return false;
        if (typeof o.speed === "number" && typeof o.timeStamp === "string") return true;
        for (const k in o) {
            let v; try { v = o[k]; } catch (_) { continue; }
            if (looksResultish(v, depth + 1)) return true;
        }
        return false;
    }

    function postSession(s) {
        try { window.postMessage({ __keybr_dash: 1, session: s }, "*"); } catch (_) {}
    }
    function postBatch(arr) {
        if (arr.length === 1) return postSession(arr[0]);
        try { window.postMessage({ __keybr_dash: 1, batch: arr }, "*"); } catch (_) {}
    }
    // Route a one-line diagnostic to the native host log.
    function diag(msg) {
        try { window.postMessage({ __keybr_dash: 1, diag: String(msg).slice(0, 400) }, "*"); } catch (_) {}
    }

    // timeStamps we've already forwarded this session — avoids re-sending the
    // whole history on every poll (the host also dedups, but this saves traffic).
    const seenTs = new Set();
    function send(results) {
        const fresh = results.filter(r => r && r.timeStamp && !seenTs.has(r.timeStamp));
        for (const r of fresh) seenTs.add(r.timeStamp);
        if (fresh.length) postBatch(fresh);
        return fresh.length;
    }

    // Pull every STRICT result out of a parsed JSON value and forward it.
    function harvest(value) {
        const items = Array.isArray(value) ? value : [value];
        const out = [];
        for (const it of items) { const r = findResult(it, 0); if (r) out.push(r); }
        return send(out);
    }

    function harvestText(txt, tag) {
        if (!txt || typeof txt !== "string") return;
        if (txt.length > 8_000_000) return;            // don't choke on huge bodies
        let j;
        try { j = JSON.parse(txt); } catch (_) { return; }
        const n = harvest(j);
        // DIAGNOSTIC: a result-ish payload we did NOT strictly capture — report it.
        if (!n && tag && looksResultish(j, 0)) {
            diag(tag + " — result-ish payload but no strict histogram match: "
                + txt.slice(0, 200));
        }
    }

    // Turn any request/beacon/socket body into text and harvest it. Handles the
    // shapes keybr might use besides a plain JSON string.
    function harvestBody(body, tag) {
        try {
            if (body == null) return;
            if (typeof body === "string") { harvestText(body, tag); return; }
            if (body instanceof ArrayBuffer) {
                harvestText(new TextDecoder().decode(body), tag); return;
            }
            if (ArrayBuffer.isView(body)) {
                harvestText(new TextDecoder().decode(body.buffer), tag); return;
            }
            if (typeof Blob !== "undefined" && body instanceof Blob) {
                body.text().then(t => harvestText(t, tag)).catch(() => {}); return;
            }
            if (typeof URLSearchParams !== "undefined" && body instanceof URLSearchParams) {
                for (const v of body.values()) harvestText(v, tag);
                return;
            }
            if (typeof FormData !== "undefined" && body instanceof FormData) {
                for (const v of body.values()) if (typeof v === "string") harvestText(v, tag);
                return;
            }
        } catch (_) {}
    }

    // ── IndexedDB poll (bulletproof, schema-agnostic) ─────────────────────────
    // keybr is local-first: a completed lesson may land in IndexedDB regardless
    // of how/whether it's also synced to the server. Every few seconds we scan
    // all of the page's IDB databases/stores for result-shaped records.
    async function pollIDB() {
        try {
            if (!window.indexedDB || !indexedDB.databases) return;
            const dbs = await indexedDB.databases();
            for (const info of dbs) {
                if (!info.name) continue;
                await new Promise((resolve) => {
                    let req;
                    try { req = indexedDB.open(info.name); } catch (_) { return resolve(); }
                    req.onerror = () => resolve();
                    req.onsuccess = () => {
                        const db = req.result;
                        const stores = Array.from(db.objectStoreNames);
                        if (!stores.length) { db.close(); return resolve(); }
                        const found = [];
                        let pending = stores.length;
                        const done = () => { db.close(); send(found); resolve(); };
                        let tx;
                        try { tx = db.transaction(stores, "readonly"); }
                        catch (_) { db.close(); return resolve(); }
                        for (const s of stores) {
                            let g;
                            try { g = tx.objectStore(s).getAll(); }
                            catch (_) { if (--pending === 0) done(); continue; }
                            g.onsuccess = () => {
                                for (const rec of g.result) {
                                    const r = findResult(rec, 0);
                                    if (r) found.push(r);
                                }
                                if (--pending === 0) done();
                            };
                            g.onerror = () => { if (--pending === 0) done(); };
                        }
                    };
                });
            }
        } catch (_) {}
    }
    setInterval(pollIDB, 3000);
    setTimeout(pollIDB, 1500);

    // ── XMLHttpRequest ────────────────────────────────────────────────────────
    try {
        const open = XMLHttpRequest.prototype.open;
        const send = XMLHttpRequest.prototype.send;
        XMLHttpRequest.prototype.open = function (m, u) {
            this.__kb_url = u; this.__kb_method = m;
            return open.apply(this, arguments);
        };
        XMLHttpRequest.prototype.send = function (body) {
            try {
                const meth = (this.__kb_method || "GET").toUpperCase();
                if (meth !== "GET" && meth !== "HEAD") {
                    diag("XHR " + meth + " " + this.__kb_url + " body:" + (body && body.constructor ? body.constructor.name : typeof body));
                    harvestBody(body, "XHR-req");
                }
            } catch (_) {}
            this.addEventListener("load", () => {
                try {
                    const ct = (this.getResponseHeader("content-type") || "");
                    if (this.responseType === "" || this.responseType === "text") {
                        if (ct.includes("json")) harvestText(this.responseText, "XHR-res");
                    } else if (this.responseType === "json" && this.response) {
                        harvest(this.response);
                    }
                } catch (_) {}
            });
            return send.apply(this, arguments);
        };
    } catch (_) {}

    // ── fetch ─────────────────────────────────────────────────────────────────
    try {
        const _fetch = window.fetch;
        if (_fetch) {
            window.fetch = function (input, init) {
                try {
                    const meth = ((init && init.method) || (input && input.method) || "GET").toUpperCase();
                    const url = (typeof input === "string") ? input : (input && input.url) || "";
                    if (meth !== "GET" && meth !== "HEAD") {
                        const b = init && init.body;
                        diag("fetch " + meth + " " + url + " body:" + (b && b.constructor ? b.constructor.name : typeof b));
                        harvestBody(b, "fetch-req");
                    }
                } catch (_) {}
                return _fetch.apply(this, arguments).then((res) => {
                    try {
                        const ct = res.headers.get("content-type") || "";
                        if (ct.includes("json")) res.clone().text().then(t => harvestText(t, "fetch-res")).catch(() => {});
                    } catch (_) {}
                    return res;
                });
            };
        }
    } catch (_) {}

    // ── navigator.sendBeacon (the classic "save on finish/unload" channel) ─────
    try {
        if (navigator.sendBeacon) {
            const _beacon = navigator.sendBeacon.bind(navigator);
            navigator.sendBeacon = function (url, data) {
                try {
                    diag("beacon " + url + " body:" + (data && data.constructor ? data.constructor.name : typeof data));
                    harvestBody(data, "beacon");
                } catch (_) {}
                return _beacon(url, data);
            };
        }
    } catch (_) {}

    // ── WebSocket (some apps stream results over a socket) ─────────────────────
    try {
        const NativeWS = window.WebSocket;
        if (NativeWS) {
            const Wrapped = function (url, protocols) {
                const ws = protocols === undefined ? new NativeWS(url) : new NativeWS(url, protocols);
                try {
                    ws.addEventListener("message", (ev) => {
                        try { harvestBody(ev.data, "ws-msg"); } catch (_) {}
                    });
                } catch (_) {}
                const _send = ws.send.bind(ws);
                ws.send = function (data) {
                    try {
                        diag("ws-send " + url + " body:" + (data && data.constructor ? data.constructor.name : typeof data));
                        harvestBody(data, "ws-send");
                    } catch (_) {}
                    return _send(data);
                };
                return ws;
            };
            Wrapped.prototype = NativeWS.prototype;
            Wrapped.CONNECTING = NativeWS.CONNECTING; Wrapped.OPEN = NativeWS.OPEN;
            Wrapped.CLOSING = NativeWS.CLOSING; Wrapped.CLOSED = NativeWS.CLOSED;
            window.WebSocket = Wrapped;
        }
    } catch (_) {}

    // ── IndexedDB writes (the local cache keybr always writes, signed in or not)
    try {
        const add = IDBObjectStore.prototype.add;
        const put = IDBObjectStore.prototype.put;
        IDBObjectStore.prototype.add = function (v) {
            try { const r = findResult(v, 0); if (r) postSession(r); } catch (_) {}
            return add.apply(this, arguments);
        };
        IDBObjectStore.prototype.put = function (v) {
            try { const r = findResult(v, 0); if (r) postSession(r); } catch (_) {}
            return put.apply(this, arguments);
        };
    } catch (_) {}

    diag("inject.js loaded (hooks: XHR+fetch+beacon+WebSocket+IDB) on " + location.href);
})();
