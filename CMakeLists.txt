cmake_minimum_required(VERSION 3.20)
project(keybr-ws CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build by default for performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
# We include ImGui as source (not a library) — this is the intended way to use it.
# The backends (GLFW + OpenGL3) are also source files we compile directly.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ── ImPlot ────────────────────────────────────────────────────────────────────
# Charting library that plugs directly into ImGui.
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.17
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(implot)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
# Header-only JSON library. Used by both binaries.
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

# ── System packages ───────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
find_package(glfw3  REQUIRED)

# ── keybr-dash ────────────────────────────────────────────────────────────────
# The stats dashboard: ImGui + ImPlot rendered via GLFW + OpenGL3
add_executable(keybr-dash
    src/dash/main.cpp

    # ImGui core
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    # ImGui backends for GLFW and OpenGL3
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp

    # ImPlot core + item renderers
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(keybr-dash PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
)

target_link_libraries(keybr-dash PRIVATE
    OpenGL::GL
    glfw
    nlohmann_json::nlohmann_json
    pthread
)

# ── keybr-host ────────────────────────────────────────────────────────────────
# The native messaging host: tiny binary, no windowing deps needed
add_executable(keybr-host
    src/host/main.cpp
)

target_link_libraries(keybr-host PRIVATE
    nlohmann_json::nlohmann_json
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS keybr-dash keybr-host
    RUNTIME DESTINATION bin
)
