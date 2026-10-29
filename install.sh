#!/usr/bin/env bash
# install.sh - build and install keybr-ws for the current user.
#
#   ./install.sh [--prefix DIR]     (default prefix: ~/.local)
#
# 1. checks deps  2. builds keybr-dash + keybr-host  3. installs the binaries,
# launch scripts and the unpacked extension  4. writes the native-messaging
# host manifest, computing the extension ID Chromium derives from the
# extension's path (so there is nothing to copy-paste).
set -euo pipefail

PREFIX="${HOME}/.local"
[[ "${1:-}" == "--prefix" ]] && PREFIX="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$PREFIX/bin"
EXT_DIR="$PREFIX/share/keybr-ext"
DATA_DIR="$PREFIX/share/keybr"
NM_DIR="${KEYBR_NM_DIR:-$HOME/.config/chromium/NativeMessagingHosts}"

info() { echo "[keybr-ws] $*"; }
die()  { echo "[keybr-ws] ERROR: $*" >&2; exit 1; }

missing=()
for c in cmake g++ git; do command -v "$c" >/dev/null || missing+=("$c"); done
pkg-config --exists glfw3 2>/dev/null || missing+=("glfw")
((${#missing[@]})) && die "missing: ${missing[*]} (Arch: sudo pacman -S cmake gcc git glfw)"

info "building (Release; ImGui, ImPlot, nlohmann/json are fetched by CMake)"
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$HERE/build" --parallel "$(nproc)"
cmake --install "$HERE/build"

info "installing scripts and extension"
mkdir -p "$BIN" "$EXT_DIR" "$DATA_DIR"
install -m755 "$HERE"/scripts/keybr-* "$HERE/tools/wal-server" "$BIN/"
cp "$HERE"/ext/*.js "$HERE/ext/manifest.json" "$EXT_DIR/"

# An unpacked extension's ID is the first 32 hex chars of sha256(absolute path),
# with 0-9a-f mapped to a-p.
EXT_REAL="$(readlink -f "$EXT_DIR")"
EXT_ID="$(printf %s "$EXT_REAL" | sha256sum | cut -c1-32 | tr '0-9a-f' 'a-p')"

mkdir -p "$NM_DIR"
sed -e "s#@HOST_PATH@#$BIN/keybr-host#" -e "s#@EXTENSION_ID@#$EXT_ID#" \
    "$HERE/config/com.keybr.host.json.template" > "$NM_DIR/com.keybr.host.json"
info "native-messaging manifest: $NM_DIR/com.keybr.host.json (extension $EXT_ID)"

cat <<MSG

Done. Next:
  * Run $BIN/keybr-launch  (starts the dashboard and the keybr.com window), or
    start them yourself; the extension path Chromium must load is:
      $EXT_DIR
  * Hyprland: add the snippet from docs/hyprland-scratchpad.lua and reload.
  * Seed history (optional): put a keybr "download data" export at
      $DATA_DIR/typing-data.json
MSG
