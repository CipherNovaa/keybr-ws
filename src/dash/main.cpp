// ============================================================
// keybr-dash  —  typing stats dashboard
// Dear ImGui + ImPlot, rendered via GLFW + OpenGL3
//
// Layout (fills top half of ws7 via Hyprland windowrule):
//   ┌─────────────────────────────────────────────────────┐
//   │  [best WPM]  [latest WPM]  [avg WPM]  [sessions]   │
//   │                                                     │
//   │         WPM line chart (all sessions)               │
//   │                                                     │
//   │  ▌▌▌▌▌▌▌▌▌▌▌  today bar strip                      │
//   └─────────────────────────────────────────────────────┘
//
// Live updates:
//   - inotify on typing-data.json  → redraws on new lesson
//   - inotify on wal/colors.json   → redraws with new palette
// ============================================================

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/inotify.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

// ── File paths ───────────────────────────────────────────────────────────────
// getenv("HOME") resolves the user's home at runtime, no hardcoding needed.
const std::string HOME        = std::string(getenv("HOME"));
const std::string DATA_PATH   = HOME + "/.local/share/keybr/typing-data.json";
const std::string COLORS_PATH = HOME + "/.cache/wal/colors.json";
const std::string DATA_DIR    = HOME + "/.local/share/keybr";
const std::string DATA_NAME   = "typing-data.json";
const std::string COLORS_DIR  = HOME + "/.cache/wal";
const std::string COLORS_NAME = "colors.json";

// ============================================================
// DATA STRUCTURES
// ============================================================

// One completed keybr lesson
struct Session {
    double      speed;     // WPM as reported by keybr
    double      accuracy;  // 0–100, computed from histogram
    std::string date;      // "YYYY-MM-DD" from the ISO timestamp
    bool        isToday;
};

// Aggregated stats built from all sessions
struct Stats {
    double best      = 0;
    double latest    = 0;
    double avgAll    = 0;
    double avgToday  = 0;
    double accAll    = 0;
    double accToday  = 0;
    int    total     = 0;
    int    todayCount = 0;
    std::vector<Session> sessions;
};

// pywal colour palette — mirrors the structure of colors.json
struct WalColors {
    ImVec4 bg;         // special.background
    ImVec4 fg;         // special.foreground
    ImVec4 accent;     // color4  — blue, used for today's line + bars
    ImVec4 accent2;    // color2  — green, available for extra elements
    ImVec4 highlight;  // color1  — warm red/orange, used for best marker
    ImVec4 muted;      // color8  — subdued text and past session line
    ImVec4 card;       // bg slightly lightened — card backgrounds
    ImVec4 border;     // bg a bit more lightened — card borders
};

// ============================================================
// HELPERS
// ============================================================

// Convert a pywal hex string like "#1a2b3c" to an ImVec4 (r,g,b,1.0)
ImVec4 hexToVec4(const std::string& hex) {
    const std::string h = (hex[0] == '#') ? hex.substr(1) : hex;
    unsigned int r = 0, g = 0, b = 0;
    sscanf(h.c_str(), "%02x%02x%02x", &r, &g, &b);
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

// Nudge a colour toward white by 'amount' (0.0–1.0)
ImVec4 lighten(ImVec4 c, float amount) {
    return ImVec4(
        std::min(c.x + amount, 1.0f),
        std::min(c.y + amount, 1.0f),
        std::min(c.z + amount, 1.0f),
        c.w
    );
}

// Today's date as "YYYY-MM-DD" in UTC (matches keybr's timestamp format)
std::string todayDate() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const std::tm* tm = std::gmtime(&t);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
    return std::string(buf);
}

// ============================================================
// JSON LOADING
// ============================================================

// Read pywal's colors.json and build a WalColors.
// Falls back to sensible dark defaults if the file is missing or broken.
WalColors loadColors() {
    WalColors c;

    // defaults — a generic dark palette so the app looks fine without pywal
    c.bg        = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    c.fg        = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    c.accent    = ImVec4(0.22f, 0.48f, 0.85f, 1.0f);
    c.accent2   = ImVec4(0.40f, 0.73f, 0.47f, 1.0f);
    c.highlight = ImVec4(0.85f, 0.36f, 0.20f, 1.0f);
    c.muted     = ImVec4(0.42f, 0.42f, 0.46f, 1.0f);
    c.card      = lighten(c.bg, 0.04f);
    c.border    = lighten(c.bg, 0.09f);

    std::ifstream f(COLORS_PATH);
    if (!f.is_open()) return c;

    try {
        const json j = json::parse(f);
        c.bg        = hexToVec4(j["special"]["background"].get<std::string>());
        c.fg        = hexToVec4(j["special"]["foreground"].get<std::string>());
        c.accent    = hexToVec4(j["colors"]["color4"].get<std::string>());
        c.accent2   = hexToVec4(j["colors"]["color2"].get<std::string>());
        c.highlight = hexToVec4(j["colors"]["color1"].get<std::string>());
        c.muted     = hexToVec4(j["colors"]["color8"].get<std::string>());
        c.card      = lighten(c.bg, 0.04f);
        c.border    = lighten(c.bg, 0.09f);
    } catch (...) {}  // keep defaults on any parse error

    return c;
}

// Parse the keybr JSON export and compute all stats.
Stats loadStats() {
    Stats s;

    std::ifstream f(DATA_PATH);
    if (!f.is_open()) return s;

    try {
        const json j   = json::parse(f);
        const std::string today = todayDate();

        double sumAll    = 0, sumToday   = 0;
        double hitsAll   = 0, missesAll  = 0;
        double hitsToday = 0, missesToday = 0;

        for (const auto& entry : j) {
          try {                       // skip a malformed row instead of dropping
            Session sess;             // the whole history (rows now arrive live)

            // keybr timestamps look like "2026-05-23T20:57:31.000Z"
            // substr(0,10) gives us just "2026-05-23"
            const std::string ts = entry.at("timeStamp").get<std::string>();
            sess.date    = ts.substr(0, 10);
            sess.speed   = entry.at("speed").get<double>() / 5.0;
            sess.isToday = (sess.date == today);

            // accuracy = hits / (hits + misses) across all keys in this session
            double hits = 0, misses = 0;
            if (entry.contains("histogram") && entry["histogram"].is_array()) {
                for (const auto& h : entry["histogram"]) {
                    hits   += h.value("hitCount", 0.0);
                    misses += h.value("missCount", 0.0);
                }
            }
            sess.accuracy = (hits + misses > 0)
                              ? (hits / (hits + misses)) * 100.0
                              : 100.0;

            s.sessions.push_back(sess);

            sumAll   += sess.speed;
            hitsAll  += hits;
            missesAll += misses;

            if (sess.isToday) {
                sumToday    += sess.speed;
                hitsToday   += hits;
                missesToday += misses;
                s.todayCount++;
            }
          } catch (...) { continue; }  // bad row → skip, keep the rest
        }

        s.total  = (int)s.sessions.size();
        s.latest = s.total > 0 ? s.sessions.back().speed : 0;

        // find best WPM across all sessions
        s.best = 0;
        for (const auto& sess : s.sessions)
            if (sess.speed > s.best) s.best = sess.speed;

        s.avgAll   = s.total      > 0 ? sumAll   / s.total      : 0;
        s.avgToday = s.todayCount > 0 ? sumToday / s.todayCount : 0;

        s.accAll   = (hitsAll   + missesAll   > 0)
                       ? hitsAll   / (hitsAll   + missesAll)   * 100.0 : 100.0;
        s.accToday = (hitsToday + missesToday > 0)
                       ? hitsToday / (hitsToday + missesToday) * 100.0 : 100.0;

    } catch (...) {}

    return s;
}

// ============================================================
// INOTIFY WATCHER  —  background thread
//
// inotify is a Linux kernel feature that lets us watch files for
// changes without polling. When either watched file is written,
// we set an atomic flag. The main (render) thread checks these
// flags every frame and reloads only when something actually changed.
// This keeps CPU at zero while idle.
// ============================================================

std::atomic<bool> g_running      {true};
std::atomic<bool> g_dataChanged  {false};
std::atomic<bool> g_colorsChanged{false};

void watcherThread() {
    // IN_NONBLOCK: reads won't block if no events are ready
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return;

    // Watch the DIRECTORIES, not the files. Both typing-data.json and
    // colors.json are rewritten via atomic rename (write .tmp, rename over),
    // which swaps the inode — a watch on the file path would go stale after the
    // first write and live updates would silently stop. A directory watch keeps
    // catching the filename across inode swaps.
    //   IN_CLOSE_WRITE: in-place writes;  IN_MOVED_TO/IN_CREATE: atomic renames.
    const uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;

    int wd1 = inotify_add_watch(fd, DATA_DIR.c_str(),   mask);
    int wd2 = inotify_add_watch(fd, COLORS_DIR.c_str(), mask);

    // buffer must be aligned for inotify_event structs
    alignas(struct inotify_event) char buf[4096];

    while (g_running) {
        const ssize_t len = read(fd, buf, sizeof(buf));

        if (len > 0) {
            const char* ptr = buf;
            while (ptr < buf + len) {
                const auto* ev = reinterpret_cast<const struct inotify_event*>(ptr);

                if (ev->len > 0) {
                    const std::string name(ev->name);
                    if (ev->wd == wd1 && name == DATA_NAME)   g_dataChanged   = true;
                    if (ev->wd == wd2 && name == COLORS_NAME) g_colorsChanged = true;
                }

                // each event is a fixed header + variable-length name field
                ptr += sizeof(struct inotify_event) + ev->len;
            }
        }

        // poll every 200ms — imperceptibly fast but negligible CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    close(fd);
}

// ============================================================
// IMGUI STYLING  —  push the pywal palette into ImGui's style
// ============================================================

void applyStyle(const WalColors& c) {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding  = 0.0f;   // outer window: no rounding (fills the screen)
    s.ChildRounding   = 8.0f;   // inner cards: rounded
    s.FrameRounding   = 4.0f;
    s.WindowPadding   = ImVec2(20, 20);
    s.ItemSpacing     = ImVec2(12, 10);
    s.FramePadding    = ImVec2(10,  6);
    s.ScrollbarSize   = 8.0f;

    ImVec4* col = s.Colors;
    col[ImGuiCol_WindowBg]         = c.bg;
    col[ImGuiCol_ChildBg]          = c.card;
    col[ImGuiCol_Border]           = c.border;
    col[ImGuiCol_Text]             = c.fg;
    col[ImGuiCol_TextDisabled]     = c.muted;
    col[ImGuiCol_FrameBg]          = c.card;
    col[ImGuiCol_FrameBgHovered]   = lighten(c.card, 0.04f);
    col[ImGuiCol_TitleBg]          = c.bg;
    col[ImGuiCol_TitleBgActive]    = c.bg;
    col[ImGuiCol_ScrollbarBg]      = c.bg;
    col[ImGuiCol_ScrollbarGrab]    = c.muted;
    col[ImGuiCol_PlotLines]        = c.accent;
    col[ImGuiCol_PlotHistogram]    = c.accent;
    col[ImGuiCol_Separator]        = c.border;
}

// ============================================================
// RENDER HELPERS
// ============================================================

// Draw a single metric card using an ImGui child window.
// 'id'    — unique ImGui ID string (e.g. "##card_best")
// 'label' — small dim label at the top
// 'value' — large number text
// 'sub'   — small subtitle below the number
void renderCard(const char* id, const char* label, const char* value,
                const char* sub, ImVec2 size, const WalColors& c)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.card);
    ImGui::PushStyleColor(ImGuiCol_Border,  c.border);

    ImGui::BeginChild(id, size, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // dim label
    ImGui::PushStyleColor(ImGuiCol_Text, c.muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // big value — SetWindowFontScale scales the font relative to the window
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted(value);
    ImGui::SetWindowFontScale(1.0f);

    // subtitle
    ImGui::PushStyleColor(ImGuiCol_Text, c.muted);
    ImGui::TextUnformatted(sub);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ============================================================
// MAIN
// ============================================================

int main() {
    // ── GLFW init ─────────────────────────────────────────────────────────────
    if (!glfwInit()) return 1;

    // Request OpenGL 3.3 core — minimum version the ImGui GL3 backend needs
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "keybr-dash");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // The Hyprland windowrule will maximize this to the top half of ws7.
    // The initial size just needs to be non-zero — Hyprland resizes it.
    GLFWwindow* win = glfwCreateWindow(1280, 540, "keybr-dash", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(win);
    // No vsync. With vsync on, glfwSwapBuffers BLOCKS on the compositor's frame
    // callback — and an occluded surface (when you're on another workspace) never
    // gets one, so the loop stalls, stops answering Wayland pings, and Hyprland
    // pops its "Application Not Responding" dialog even though the app is fine.
    // swapInterval(0) makes present non-blocking; we throttle via the event wait.
    glfwSwapInterval(0);

    // ── ImGui + ImPlot init ───────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't write/read imgui.ini

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ── initial load ──────────────────────────────────────────────────────────
    WalColors colors = loadColors();
    Stats     stats  = loadStats();
    applyStyle(colors);

    // ── start inotify watcher on a background thread ──────────────────────────
    std::thread watcher(watcherThread);

    // ── main render loop ──────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(win)) {
        const auto frame_start = std::chrono::steady_clock::now();
        // Wait up to 100ms for events, but wake immediately on input or a
        // compositor ping. This pumps the Wayland queue (keeping us responsive
        // even while occluded) and caps idle redraws to ~10fps for low CPU.
        glfwWaitEventsTimeout(0.1);

        // check if inotify flagged any changes since last frame
        // exchange(false) atomically reads the flag and resets it to false
        if (g_dataChanged.exchange(false))    stats  = loadStats();
        if (g_colorsChanged.exchange(false)) {
            colors = loadColors();
            applyStyle(colors);
        }

        // ── start new ImGui frame ─────────────────────────────────────────────
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // get the actual framebuffer size (respects HiDPI if ever relevant)
        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        const float W = (float)fbW;
        const float H = (float)fbH;

        // full-screen panel — no title bar, no resize, no move
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── metric cards ──────────────────────────────────────────────────────
        // 4 cards side by side, with padding between and around them
        const float pad   = 12.0f;
        const float cardW = (W - (5.0f * pad)) / 4.0f;  // 5 gaps: L edge, 3 between, R edge
        const float cardH = 120.0f;

        char val[32], sub[48];

        // card 1 — best WPM
        ImGui::SetCursorPos(ImVec2(pad, pad));
        snprintf(val, sizeof(val), "%.0f", stats.best);
        renderCard("##c1", "best speed", val, "WPM all time", ImVec2(cardW, cardH), colors);

        // card 2 — latest
        ImGui::SameLine(0, pad);
        snprintf(val, sizeof(val), "%.0f", stats.latest);
        renderCard("##c2", "latest", val, "WPM last session", ImVec2(cardW, cardH), colors);

        // card 3 — average (prefer today's avg if we have today sessions)
        ImGui::SameLine(0, pad);
        const bool hasToday = stats.todayCount > 0;
        snprintf(val, sizeof(val), "%.0f", hasToday ? stats.avgToday : stats.avgAll);
        snprintf(sub, sizeof(sub), "WPM %s avg", hasToday ? "today" : "all-time");
        renderCard("##c3", "average", val, sub, ImVec2(cardW, cardH), colors);

        // card 4 — session count
        ImGui::SameLine(0, pad);
        snprintf(val, sizeof(val), "%d", stats.total);
        snprintf(sub, sizeof(sub), "%d today", stats.todayCount);
        renderCard("##c4", "sessions", val, sub, ImVec2(cardW, cardH), colors);

        // ── ImPlot: WPM line chart ─────────────────────────────────────────────
        // Reserve height for card row + bar strip + padding; chart fills the rest.
        const float barStripH = stats.todayCount > 0 ? 60.0f : 0.0f;
        const float plotY     = cardH + (pad * 2.0f);
        const float plotH     = H - plotY - barStripH - (pad * 2.0f);

        ImGui::SetCursorPos(ImVec2(pad, plotY));

        // ImPlot flags: no title (we don't need a header above the chart)
        if (ImPlot::BeginPlot("##wpm_chart", ImVec2(W - pad * 2.0f, plotH),
                              ImPlotFlags_NoTitle))
        {
            // X axis — session index; Y axis — WPM.
            // AutoFit: ImPlot adjusts range to fit data.
            ImPlot::SetupAxes("", "WPM",
                ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel,
                ImPlotAxisFlags_AutoFit);

            // Split into past and today vectors for separate line colours.
            // ImPlot needs contiguous arrays so we use separate vectors.
            std::vector<double> xPast, yPast, xToday, yToday;
            for (int i = 0; i < (int)stats.sessions.size(); i++) {
                const auto& sess = stats.sessions[i];
                if (sess.isToday) {
                    xToday.push_back(i);
                    yToday.push_back(sess.speed);
                } else {
                    xPast.push_back(i);
                    yPast.push_back(sess.speed);
                }
            }

            // past sessions — muted/dim colour, thinner line
            if (!xPast.empty()) {
                ImPlot::SetNextLineStyle(
                    ImVec4(colors.muted.x, colors.muted.y, colors.muted.z, 0.75f),
                    1.8f);
                ImPlot::PlotLine("past", xPast.data(), yPast.data(),
                                 (int)xPast.size());
            }

            // today's sessions — accent colour, slightly thicker
            if (!xToday.empty()) {
                ImPlot::SetNextLineStyle(colors.accent, 2.5f);
                ImPlot::PlotLine("today", xToday.data(), yToday.data(),
                                 (int)xToday.size());
            }

            // best session — highlighted dot in accent/warm colour
            if (stats.best > 0) {
                for (int i = 0; i < (int)stats.sessions.size(); i++) {
                    if (stats.sessions[i].speed == stats.best) {
                        double bx = (double)i;
                        double by = stats.best;
                        ImPlot::SetNextMarkerStyle(
                            ImPlotMarker_Circle, 9.0f,
                            colors.highlight, 2.0f, colors.highlight);
                        ImPlot::PlotScatter("##best", &bx, &by, 1);
                        break;
                    }
                }
            }

            ImPlot::EndPlot();
        }

        // ── today bar strip ───────────────────────────────────────────────────
        // Drawn with the raw draw list (not ImGui widgets) for full control.
        const float stripY = H - barStripH - pad;
        ImGui::SetCursorPos(ImVec2(pad, stripY - 20.0f));

        // small label above the bars
        ImGui::PushStyleColor(ImGuiCol_Text, colors.muted);
        if (stats.todayCount > 0) ImGui::Text("today  —  %d sessions", stats.todayCount);
        ImGui::PopStyleColor();

        if (stats.todayCount > 0) {
            // find today's max for scaling bars to fill the strip height
            double todayMax = 0;
            for (const auto& sess : stats.sessions)
                if (sess.isToday && sess.speed > todayMax) todayMax = sess.speed;

            auto* dl       = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float  barW   = 10.0f;
            const float  gap    =  3.0f;
            const float  maxH   = barStripH - 8.0f;
            float        x      = origin.x;

            for (const auto& sess : stats.sessions) {
                if (!sess.isToday) continue;

                const float ratio = (todayMax > 0)
                                      ? (float)(sess.speed / todayMax)
                                      : 0.0f;
                const float bh  = maxH * ratio;

                // best session gets the highlight colour; others get accent
                const ImVec4 col4 = (sess.speed == stats.best)
                                      ? colors.highlight
                                      : colors.accent;
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);

                dl->AddRectFilled(
                    ImVec2(x,        origin.y + maxH - bh),
                    ImVec2(x + barW, origin.y + maxH),
                    col, 2.0f  // 2px corner rounding
                );

                x += barW + gap;
            }
        }

        ImGui::End();

        // ── render ────────────────────────────────────────────────────────────
        ImGui::Render();
        glClearColor(colors.bg.x, colors.bg.y, colors.bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);

        // Frame-rate cap (~30fps). glfwSwapInterval(0) (no vsync, needed to avoid
        // blocking when occluded) means that when the compositor floods frame
        // events the loop runs far faster than the intended 10fps idle, pinning a
        // CPU core (~58% on the laptop → 70°C). This sleeps out the rest of each
        // frame period so CPU stays low regardless of event rate. Added 2026-07-13.
        const auto frame_min = std::chrono::milliseconds(33);
        const auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < frame_min)
            std::this_thread::sleep_for(frame_min - elapsed);
    }

    // ── cleanup ───────────────────────────────────────────────────────────────
    g_running = false;    // signals the watcher thread to exit its loop
    watcher.join();       // wait for it to finish before destroying anything

    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();

    return 0;
}
