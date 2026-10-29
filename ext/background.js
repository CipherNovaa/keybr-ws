// background.js — Chromium Manifest V3 service worker
//
// Two responsibilities:
//   1. Receive lesson_complete messages from content.js,
//      forward to keybr-host via native messaging.
//   2. Fetch pywal colors from keybr-host and relay them
//      to content.js for CSS theming.

const HOST = "com.keybr.host";

// ── 1. Message router ─────────────────────────────────────────────────────────

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {

    if (msg.type === "lesson_complete") {
        // Send the session object to our C++ host.
        // sendNativeMessage is fire-and-forget for MV3 (no persistent connection).
        chrome.runtime.sendNativeMessage(HOST, msg.data, (response) => {
            if (chrome.runtime.lastError) {
                console.warn("[keybr-dash] native messaging error:",
                    chrome.runtime.lastError.message);
            }
            // response is {"status":"ok"} or {"status":"error",...}
        });
        return false; // no async sendResponse needed
    }

    if (msg.type === "diag") {
        // Outgoing-call diagnostics from inject.js → host log (/tmp/keybr-host.log).
        chrome.runtime.sendNativeMessage(HOST, { type: "diag", text: msg.data }, () => {
            void chrome.runtime.lastError; // fire-and-forget
        });
        return false;
    }

    if (msg.type === "sync_batch") {
        // A batch of sessions (e.g. the full history fetched on page load).
        // The host merges + dedups by timeStamp in a single atomic write.
        chrome.runtime.sendNativeMessage(HOST, { type: "sync_batch", sessions: msg.data }, (response) => {
            if (chrome.runtime.lastError) {
                console.warn("[keybr-dash] native messaging error:",
                    chrome.runtime.lastError.message);
            }
        });
        return false;
    }

    if (msg.type === "get_colors") {
        // content.js is asking for the current pywal palette.
        // We ask the host to read colors.json and return the values.
        chrome.runtime.sendNativeMessage(HOST, { type: "get_colors" }, (response) => {
            if (chrome.runtime.lastError || !response?.colors) {
                sendResponse(null);
                return;
            }
            sendResponse({ colors: response.colors });
        });
        return true; // return true to keep sendResponse valid across async call
    }

});

// ── 2. Periodic color polling ─────────────────────────────────────────────────
//
// Poll for palette changes every 30 seconds. If pywal ran and colors changed,
// push the new palette to any open keybr tabs so they retheme without reload.

let lastColorSig = "";

async function pollColors() {
    chrome.runtime.sendNativeMessage(HOST, { type: "get_colors" }, (response) => {
        if (chrome.runtime.lastError || !response?.colors) return;

        // build a cheap signature to avoid pushing when nothing changed
        const sig = JSON.stringify(response.colors);
        if (sig === lastColorSig) return;
        lastColorSig = sig;

        // push updated palette to all keybr tabs
        chrome.tabs.query({ url: "https://www.keybr.com/*" }, (tabs) => {
            for (const tab of tabs) {
                chrome.tabs.sendMessage(tab.id, {
                    type: "colors_updated",
                    colors: response.colors
                }).catch(() => {}); // tab may not have content script yet
            }
        });
    });
}

// run immediately on startup and then every 30 seconds
pollColors();
setInterval(pollColors, 30_000);
