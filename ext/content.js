// content.js — runs on https://www.keybr.com/* (isolated world)
//
//   1. Relays result-shaped data captured by inject.js (MAIN world) to the
//      background worker, which persists it via the native host → live graph.
//   2. Fetches the current pywal palette from wal-server (localhost:7799)
//      and injects it as CSS variables so keybr.com matches your rice.
//      Polls every 2s so wallpaper changes apply near-instantly.

// ── 0. Live-data relay (from inject.js running in the page's MAIN world) ──────
window.addEventListener("message", (e) => {
    if (e.source !== window) return;
    const d = e.data;
    if (!d || d.__keybr_dash !== 1) return;
    try {
        if (Array.isArray(d.batch)) {
            chrome.runtime.sendMessage({ type: "sync_batch", data: d.batch });
        } else if (d.session) {
            chrome.runtime.sendMessage({ type: "lesson_complete", data: d.session });
        } else if (typeof d.diag === "string") {
            chrome.runtime.sendMessage({ type: "diag", data: d.diag });
        }
    } catch (_) {
        // extension context invalidated (reload) — ignore
    }
});

const WAL_URL = "http://localhost:7799/";

// ── pywal CSS theming ─────────────────────────────────────────────────────────

function applyPalette(wal) {
    const sp  = wal.special;
    const col = wal.colors;

    const css = `
        :root {
            --kd-bg:        ${sp.background};
            --kd-fg:        ${sp.foreground};
            --kd-accent:    ${col.color4};
            --kd-accent2:   ${col.color2};
            --kd-highlight: ${col.color1};
            --kd-muted:     ${col.color8};
            --kd-card:      ${col.color0};
            /* remap keybr's own brand vars (the stray purple) to pywal */
            --primary-color:      ${col.color4} !important;
            --accent-color:       ${col.color4} !important;
            --focus-color:        ${col.color4} !important;
            --selected-color:     ${col.color4} !important;
            --textinput-cursor:   ${col.color4} !important;
        }
        html, body, #root, #app, [class*="App_"], [class*="Screen_"] {
            background-color: var(--kd-bg)  !important;
            color:            var(--kd-fg)  !important;
        }
        [class*="key--hit"] { background-color: var(--kd-accent) !important; }
        [class*="TextInput"][class*="cursor"] { border-color: var(--kd-accent) !important; }
        [class*="TextInput"] {
            background-color: var(--kd-card) !important;
            color:            var(--kd-fg)  !important;
        }
        /* surfaces: cards, panels, tiles, dialogs, popups, menus, sheets */
        [class*="Card"], [class*="card"], [class*="Panel"], [class*="panel"],
        [class*="Tile"], [class*="Dialog"], [class*="Popup"], [class*="Menu"],
        [class*="Sheet"], [class*="Portal"], [class*="Tooltip"], [class*="Modal"],
        [class*="Dropdown"], [class*="Popover"], dialog {
            background-color: var(--kd-card) !important;
            color:            var(--kd-fg)   !important;
            border-color:     var(--kd-muted) !important;
        }
        /* top bars / nav / toolbars */
        [class*="Header"], [class*="header"], [class*="Toolbar"], [class*="toolbar"],
        [class*="Nav"], [class*="AppBar"], header {
            background-color: var(--kd-bg) !important;
            color:            var(--kd-fg) !important;
        }
        /* buttons + form controls (the bright-white culprits) */
        button, [class*="Button"], [class*="button"], [role="button"],
        input, select, textarea, [class*="Field"], [class*="Input"] {
            background-color: var(--kd-card) !important;
            color:            var(--kd-fg)   !important;
            border-color:     var(--kd-muted) !important;
        }
        button:hover, [class*="Button"]:hover, [role="button"]:hover {
            background-color: var(--kd-accent) !important;
            color:            var(--kd-bg)     !important;
        }
        /* active / primary / selected states (keybr's purple) → pywal accent */
        [class*="primary"], [class*="Primary"], [class*="selected"], [class*="Selected"],
        [class*="active"], [class*="Active"], [aria-selected="true"], [class*="checked"] {
            background-color: var(--kd-accent) !important;
            color:            var(--kd-bg)     !important;
            border-color:     var(--kd-accent) !important;
        }
        a, [class*="Link"], [class*="link"] { color: var(--kd-accent) !important; }
        h1,h2,h3,h4,h5,h6, label, th, legend, [class*="Title"], [class*="Label"] {
            color: var(--kd-fg) !important;
        }
        svg text { fill: var(--kd-fg) !important; }
        /* Hide scrollbars (page stays scrollable) — no side scrollbar in practice. */
        ::-webkit-scrollbar { width: 0 !important; height: 0 !important; background: transparent !important; }
        html, body { scrollbar-width: none !important; }
    `;

    let tag = document.getElementById("__keybr_wal__");
    if (!tag) {
        tag = document.createElement("style");
        tag.id = "__keybr_wal__";
        (document.head || document.documentElement).appendChild(tag);
    }
    tag.textContent = css;
}

let lastSig = "";

async function fetchAndApply() {
    try {
        const res = await fetch(WAL_URL, { cache: "no-store" });
        if (!res.ok) return;
        const wal = await res.json();
        const sig = wal.special.background + wal.colors.color4;
        if (sig === lastSig) return;
        lastSig = sig;
        applyPalette(wal);
    } catch (_) {
        // wal-server not running — fail silently, keybr still works normally
    }
}

// apply immediately on page load, then poll frequently so wallpaper
// changes land near-instantly (wal-server reads colors.json fresh per request)
fetchAndApply();
setInterval(fetchAndApply, 2_000);

// ── adblock notice removal ────────────────────────────────────────────────────

const ADBLOCK_KEYWORDS = ["ad-blocker", "adblock", "remove ads", "premium account"];

function tryHide(el) {
    let node = el;
    for (let i = 0; i < 10; i++) {
        if (!node.parentElement || node === document.body) return;
        node = node.parentElement;
        const r = node.getBoundingClientRect();
        if (r.width > 80 && r.height > 30 && r.height < 400) {
            node.style.setProperty("display", "none", "important");
            return;
        }
    }
}

function scanForAdblockNotice(root) {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, null);
    let node;
    while ((node = walker.nextNode())) {
        const text = node.textContent.toLowerCase();
        if (ADBLOCK_KEYWORDS.some(k => text.includes(k))) {
            tryHide(node.parentElement);
        }
    }
}

function initialScan() {
    if (document.body) scanForAdblockNotice(document.body);
}

const adblockObserver = new MutationObserver(mutations => {
    for (const m of mutations) {
        for (const node of m.addedNodes) {
            if (node.nodeType === Node.ELEMENT_NODE) {
                scanForAdblockNotice(node);
            }
        }
    }
});

if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
        initialScan();
        adblockObserver.observe(document.body, { childList: true, subtree: true });
    });
} else {
    initialScan();
    if (document.body) {
        adblockObserver.observe(document.body, { childList: true, subtree: true });
    }
}

// ── Theme the stragglers keybr hard-codes ─────────────────────────────────────
// keybr sets some backgrounds bright-WHITE (the "All keys" unlearned tiles, the
// Daily-goal bar, some panels) and some text near-BLACK (invisible on the dark
// rice) via component styles the CSS above can't reach. This recolours them by
// COMPUTED colour — so it catches every straggler regardless of hashed class —
// while NEVER touching the functional finger-zone keyboard (those keys aren't
// white) or the green learned keys. It references the pywal CSS vars, so it also
// follows wallpaper changes. Driven by a MutationObserver (not a throttled timer),
// so it works even while keybr sits on the hidden scratchpad.
(function themeStragglers() {
    const rgb = s => (String(s).match(/[\d.]+/g) || []).map(Number);
    function fix(el) {
        if (!el || el.nodeType !== 1 || el.dataset.kdFix) return;
        let cs; try { cs = getComputedStyle(el); } catch (_) { return; }
        const [br, bg, bb, ba = 1] = rgb(cs.backgroundColor);
        const [cr, cg, cb] = rgb(cs.color);
        let touched = false;
        if (ba > 0.6 && br > 200 && bg > 200 && bb > 200) {           // near-white background
            el.style.setProperty('background-color', 'var(--kd-card, #1b1b25)', 'important');
            el.style.setProperty('border-color', 'var(--kd-muted, #444)', 'important');
            if (cr > 175 && cg > 175 && cb > 175)                     // light text on the old white
                el.style.setProperty('color', 'var(--kd-fg, #cfd6e6)', 'important');
            touched = true;
        }
        const lum = 0.299 * cr + 0.587 * cg + 0.114 * cb;
        if (cr !== undefined && lum < 55 && el.childElementCount === 0
                && el.textContent.trim()) {                          // near-invisible dark text
            el.style.setProperty('color', 'var(--kd-fg, #cfd6e6)', 'important');
            touched = true;
        }
        // SVG shapes with a near-white FILL = the virtual keyboard's plate/frame.
        // Theme it, but NEVER <text> (white key labels must stay legible) and never
        // the colourful keys (their fill isn't near-white, so they're untouched).
        if (el.namespaceURI === 'http://www.w3.org/2000/svg'
                && el.tagName && el.tagName.toLowerCase() !== 'text') {
            const [fr, fgc, fb, fa = 1] = rgb(cs.fill);
            if (fr !== undefined && fa > 0.6 && fr > 200 && fgc > 200 && fb > 200) {
                // A LIFTED dark surface — NOT pure --kd-card, which is near-black on a
                // greyscale palette and made the keyboard vanish. color-mix keeps it
                // pywal-following; the static line is the fallback.
                el.style.setProperty('fill', '#2c2c2c', 'important');
                el.style.setProperty('fill',
                    'color-mix(in srgb, var(--kd-card, #1b1b25) 74%, var(--kd-fg, #cccccc) 26%)',
                    'important');
                touched = true;
            }
        }
        if (touched) el.dataset.kdFix = '1';
    }
    function scan(root) {
        if (!root || root.nodeType !== 1) return;
        fix(root);
        let els; try { els = root.querySelectorAll('*'); } catch (_) { return; }
        for (const el of els) fix(el);
    }
    const run = () => { if (document.body) scan(document.body); };
    run();
    document.addEventListener('DOMContentLoaded', run);
    new MutationObserver(ms => { for (const m of ms) for (const n of m.addedNodes) scan(n); })
        .observe(document.documentElement, { childList: true, subtree: true });
})();
