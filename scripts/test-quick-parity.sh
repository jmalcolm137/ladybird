#!/usr/bin/env bash
# Quick-parity regression test for the keyboard paths added to the LibGUI chrome:
# Ctrl+L focuses the location bar, and Ctrl+O opens the native file picker.
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

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-quick-parity.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data" "$RUNTIME/config"
SOCK="ladybird-quick-parity-$$"

cleanup() {
    kill "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    XDG_DATA_HOME="$RUNTIME/data" \
    XDG_CONFIG_HOME="$RUNTIME/config" \
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
    LADYBIRD_SDE_PROFILE="quick-parity-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    LADYBIRD_SDE_TEST_LOG_FOCUS=1 \
    LADYBIRD_SDE_TEST_LOG_QUICK_PARITY=1 \
    "$ROOT/build/chrome/Ladybird" about:newtab >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

for _ in $(seq 1 150); do
    grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null && break
    sleep 0.1
done
sleep 3

send_keys() {
    WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 "$@" >/dev/null 2>&1
}

# Ctrl+L focuses the location bar, then Ctrl+O opens the native file picker, and
# Escape closes it again. Keep them in one virtual-input connection: destroying
# the virtual keyboard between calls drops the seat's keyboard focus.
send_keys \
    --move 640 400 --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 300 \
    --key 29 press --sleep 60 --key 38 press --sleep 60 --key 38 release --sleep 60 --key 29 release --sleep 500 \
    --key 29 press --sleep 60 --key 24 press --sleep 60 --key 24 release --sleep 60 --key 29 release --sleep 800 \
    --key 1 press --sleep 60 --key 1 release --sleep 300

if ! grep -q 'focus location' "$RUNTIME/chrome.log"; then
    echo "  FAIL: Ctrl+L did not focus the location bar"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: Ctrl+L focuses the location bar"

if ! grep -q "create_window.*title='Open File'" "$RUNTIME/chrome.log"; then
    echo "  FAIL: Ctrl+O did not open the file picker"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: Ctrl+O opens the file picker"

# View -> Always on Top, using the window's actual position.
sleep 0.5
POSITION="$(grep -o "placed new window 'New Tab - Ladybird' at [0-9]*,[0-9]*" "$RUNTIME/compositor.log" | tail -1)"
if [[ -n "$POSITION" ]]; then
    X="${POSITION##* at }"
    Y="${X##*,}"
    X="${X%%,*}"
    VIEW_X=$((X + 90))
    VIEW_Y=$((Y + 33))
    AOT_X=$((X + 100))
    AOT_Y=$((Y + 317))
    send_keys --move "$VIEW_X" "$VIEW_Y" --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 400 \
        --move "$AOT_X" "$AOT_Y" --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 600
    if ! grep -q 'always on top: true' "$RUNTIME/chrome.log" || ! grep -q 'always-on-top=true' "$RUNTIME/compositor.log"; then
        echo "  FAIL: Always on Top did not reach the compositor"
        tail -20 "$RUNTIME/chrome.log"
        exit 1
    fi
    echo "  ok: Always on Top reaches the compositor"
fi

if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log"; then
    echo "  FAIL: the browser faulted"
    exit 1
fi
echo "==> quick-parity test PASSED"
