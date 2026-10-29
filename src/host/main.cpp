// ============================================================
// keybr-host  —  Chrome native messaging host
//
// Chrome's native messaging protocol (used by our extension):
//   - stdin:  4-byte little-endian length  +  N bytes of JSON
//   - stdout: 4-byte little-endian length  +  N bytes of JSON
//
// This binary sits completely idle between lessons. It only wakes
// when the extension sends a message (after each keybr session),
// appends the new session to the JSON file, then sleeps again.
//
// Registered at:
//   ~/.config/chromium/NativeMessagingHosts/com.keybr.host.json
// ============================================================

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <set>
#include <sys/stat.h>
#include <iostream>

using json = nlohmann::json;

const std::string HOME      = std::string(getenv("HOME"));
const std::string DATA_DIR  = HOME + "/.local/share/keybr";
const std::string DATA_PATH = DATA_DIR + "/typing-data.json";
const std::string TMP_PATH  = DATA_PATH + ".tmp";

// ── Native messaging I/O ─────────────────────────────────────────────────────

// Read one message from stdin.
// Returns an empty string if stdin closes (extension unloaded/tab closed).
std::string readMessage() {
    // first, read the 4-byte length prefix
    uint32_t len = 0;
    if (!std::cin.read(reinterpret_cast<char*>(&len), 4)) return "";

    // sanity check — keybr session JSON won't be >1 MB
    if (len == 0 || len > 1024 * 1024) return "";

    std::string msg(len, '\0');
    if (!std::cin.read(msg.data(), len)) return "";
    return msg;
}

// Write one message to stdout (Chrome reads it from the other end).
void sendMessage(const std::string& msg) {
    const uint32_t len = static_cast<uint32_t>(msg.size());
    std::cout.write(reinterpret_cast<const char*>(&len), 4);
    std::cout.write(msg.data(), static_cast<std::streamsize>(len));
    std::cout.flush();
}

// ── File I/O ─────────────────────────────────────────────────────────────────

// Ensure the data directory exists (equivalent to mkdir -p)
void ensureDir() {
    mkdir(DATA_DIR.c_str(), 0755);
}

// Load the existing sessions array, or return an empty array if missing/broken.
json loadData() {
    std::ifstream f(DATA_PATH);
    if (!f.is_open()) return json::array();

    try {
        json j;
        f >> j;
        if (j.is_array()) return j;
    } catch (...) {}

    return json::array();
}

// Write the sessions array atomically.
//
// "Atomically" here means: write to a .tmp file first, then rename it
// over the real file. rename() is an atomic syscall on Linux — keybr-dash
// will never read a half-written file. This also triggers inotify's
// IN_MOVED_TO event that keybr-dash watches for.
bool saveData(const json& data) {
    ensureDir();
    std::ofstream out(TMP_PATH);
    if (!out.is_open()) return false;

    // dump(2) = pretty-print with 2-space indent
    out << data.dump(2);
    out.close();

    // atomic rename: this is what triggers inotify in keybr-dash
    return (rename(TMP_PATH.c_str(), DATA_PATH.c_str()) == 0);
}

// Build a set of the timeStamps already on disk, for dedup. Each keybr session
// is uniquely identified by its ISO "timeStamp", so the live capture (which may
// see the same result via both the network and IndexedDB, or re-see history on
// reload) stays idempotent — no duplicate rows ever land in the graph.
std::set<std::string> existingStamps(const json& data) {
    std::set<std::string> seen;
    for (const auto& s : data)
        if (s.contains("timeStamp") && s["timeStamp"].is_string())
            seen.insert(s["timeStamp"].get<std::string>());
    return seen;
}

// Append one session if its timeStamp isn't already present. Returns:
//   1 = appended+saved, 0 = duplicate (skipped), -1 = write error.
int appendAndSave(const json& session) {
    json data = loadData();
    auto seen = existingStamps(data);
    const std::string ts = session.value("timeStamp", "");
    if (!ts.empty() && seen.count(ts)) return 0;   // already have it
    data.push_back(session);
    return saveData(data) ? 1 : -1;
}

// Merge a batch of sessions (e.g. the full history fetched on page load),
// keeping only ones we don't already have. One write for the whole batch.
// Returns the number of new sessions added (or -1 on write error).
int mergeAndSave(const json& sessions) {
    json data = loadData();
    auto seen = existingStamps(data);
    int added = 0;
    for (const auto& s : sessions) {
        if (!s.is_object() || !s.contains("speed") || !s.contains("timeStamp"))
            continue;
        const std::string ts = s.value("timeStamp", "");
        if (!ts.empty() && seen.count(ts)) continue;
        data.push_back(s);
        if (!ts.empty()) seen.insert(ts);
        added++;
    }
    if (added == 0) return 0;              // nothing new; avoid a needless write
    return saveData(data) ? added : -1;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    // Chrome expects binary I/O on both stdin and stdout.
    // On Linux this is a no-op but explicit for correctness.
    std::cin.exceptions(std::ios::badbit);

    {
        std::ofstream lg("/tmp/keybr-host.log", std::ios::app);
        if (lg) lg << "[host] started\n";
    }

    while (true) {
        const std::string raw = readMessage();

        // empty string means stdin closed — extension gone, exit cleanly
        if (raw.empty()) break;

        // DIAGNOSTIC: log every message received (type + size) so we can see
        // whether the browser bridge is firing on lessons. Remove once confirmed.
        {
            std::ofstream lg("/tmp/keybr-host.log", std::ios::app);
            if (lg) {
                std::string kind = "?";
                try { auto m = json::parse(raw);
                    if (m.value("type","")=="get_colors") kind="get_colors";
                    else if (m.value("type","")=="diag") kind="DIAG "+m.value("text",std::string());
                    else if (m.value("type","")=="sync_batch") kind="sync_batch("+std::to_string(m.value("sessions",json::array()).size())+")";
                    else if (m.contains("speed")) kind="lesson_complete(speed="+std::to_string((int)m.value("speed",0.0))+")";
                } catch(...) { kind="unparseable"; }
                lg << "[host] msg " << kind << " (" << raw.size() << "b)\n";
            }
        }

        try {
            const json msg = json::parse(raw);

            // ── get_colors: extension asking for the pywal palette ────────────
            // background.js sends this on startup and every 30s.
            // We read colors.json and return the flat color map.
            if (msg.value("type", "") == "get_colors") {
                const std::string colorsPath =
                    HOME + "/.cache/wal/colors.json";
                std::ifstream cf(colorsPath);
                if (!cf.is_open()) {
                    sendMessage(R"({"colors":null})");
                    continue;
                }
                try {
                    json wal = json::parse(cf);
                    // flatten special + colors into one object for the extension
                    json palette = wal["colors"];
                    palette["background"] = wal["special"]["background"];
                    palette["foreground"] = wal["special"]["foreground"];
                    palette["cursor"]     = wal["special"]["cursor"];
                    sendMessage(json{{"colors", palette}}.dump());
                } catch (...) {
                    sendMessage(R"({"colors":null})");
                }
                continue;
            }

            // ── diag: an inject.js outgoing-call trace, already logged above ──
            if (msg.value("type", "") == "diag") {
                sendMessage(R"({"status":"ok"})");
                continue;
            }

            // ── sync_batch: a batch of sessions (e.g. full history on load) ───
            // Merge whatever we don't already have, in a single atomic write.
            if (msg.value("type", "") == "sync_batch") {
                if (!msg.contains("sessions") || !msg["sessions"].is_array()) {
                    sendMessage(R"({"status":"error","reason":"no sessions"})");
                    continue;
                }
                int added = mergeAndSave(msg["sessions"]);
                if (added < 0) sendMessage(R"({"status":"error","reason":"write failed"})");
                else sendMessage(json{{"status", "ok"}, {"added", added}}.dump());
                continue;
            }

            // ── lesson_complete: new keybr session to persist ─────────────────
            // The extension sends the full keybr session object.
            // It must have at least "speed", "timeStamp", "histogram".
            if (!msg.contains("speed") || !msg.contains("timeStamp")) {
                sendMessage(R"({"status":"error","reason":"missing fields"})");
                continue;
            }

            int r = appendAndSave(msg);   // 1 added, 0 duplicate, -1 error
            if (r >= 0) sendMessage(json{{"status", "ok"}, {"added", r}}.dump());
            else sendMessage(R"({"status":"error","reason":"write failed"})");

        } catch (const json::exception&) {
            sendMessage(R"({"status":"error","reason":"json parse failed"})");
        }
    }

    return 0;
}
