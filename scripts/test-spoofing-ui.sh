#!/usr/bin/env bash
# Spoofing-dialog regression test: selects a User-Agent preset and a navigator
# compatibility mode through the native Tools dialogs and asserts the frontend
# sent both choices to the engine.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"

[[ -x "$ROOT/build/chrome/Ladybird" && -x "$ROOT/Build/release/libexec/LadybirdEngine" ]] || {
    echo "error: build the frontend and engine first" >&2
    exit 1
}
[[ -x "$COMP" && -x "$VI" ]] || { echo "SKIP: SDE compositor or virtual input is missing"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-spoof-test.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data" "$RUNTIME/config"
SOCK="ladybird-spoof-test-$$"

cleanup() {
    kill "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$LAGOM/lib" \
    "$COMP" -n "$SOCK" >"$RUNTIME/compositor.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 100); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done

env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" \
    XDG_DATA_HOME="$RUNTIME/data" \
    XDG_CONFIG_HOME="$RUNTIME/config" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    SDE_APP_BIN_DIR="$LAGOM/bin" \
    LADYBIRD_SOURCE_DIR="$ROOT" \
    LADYBIRD_ENGINE_HOST="$ROOT/Build/release/libexec/LadybirdEngine" \
    LADYBIRD_SDE_PROFILE="spoof-test-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    LADYBIRD_SDE_TEST_LOG_SPOOFING=1 \
    "$ROOT/build/chrome/Ladybird" about:newtab >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

for _ in $(seq 1 150); do
    grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null && break
    sleep 0.1
done
sleep 2

click() {
    WAYLAND_DISPLAY="$SOCK" "$VI" --move "$1" "$2" --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 500 >/dev/null 2>&1
}

# Tools -> User Agent..., choose Chrome Linux Desktop, Apply.
click 378 133
click 500 282
sleep 0.5
click 640 400
click 640 428
sleep 0.4
click 912 466
sleep 1

if ! grep -q 'spoof user agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36' "$RUNTIME/chrome.log"; then
    echo "  FAIL: the User-Agent dialog did not send its preset"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: User-Agent dialog"

# Tools -> Navigator Compatibility Mode..., choose Gecko, Apply.
click 378 133
click 500 306
sleep 0.5
click 640 413
click 400 451
sleep 0.4
click 872 446
sleep 1

if ! grep -q 'spoof navigator: gecko' "$RUNTIME/chrome.log"; then
    echo "  FAIL: the navigator dialog did not send its mode"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: Navigator Compatibility dialog"

if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log"; then
    echo "  FAIL: the browser faulted"
    exit 1
fi
echo "==> spoofing-dialog test PASSED"
