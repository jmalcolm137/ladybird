#!/usr/bin/env bash
# Exit-reason regression test: a SIGTERM and the Quit action each leave a
# distinguishable line in the session log. Without it, a quit and a window that
# disappeared look identical, and a real crash gives no trace at all.
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

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-exit-reason.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data" "$RUNTIME/config"
SOCK="ladybird-exit-reason-$$"

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

start_browser() {
    local log="$1"
    local mapped_before
    mapped_before="$(grep -c 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null || true)"
    env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" \
        XDG_DATA_HOME="$RUNTIME/data" \
        XDG_CONFIG_HOME="$RUNTIME/config" \
        SERENITY_RES_ROOT="$SERENITY/Base/res" \
        SDE_APP_BIN_DIR="$LAGOM/bin" \
        LADYBIRD_SOURCE_DIR="$ROOT" \
        LADYBIRD_ENGINE_HOST="$ROOT/Build/release/libexec/LadybirdEngine" \
        LADYBIRD_SDE_PROFILE="exit-reason-$$" \
        LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
        "$ROOT/build/chrome/Ladybird" about:newtab >"$log" 2>&1 &
    CHROME_PID=$!
    for _ in $(seq 1 150); do
        local mapped_now
        mapped_now="$(grep -c 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null || true)"
        [[ "$mapped_now" -gt "$mapped_before" ]] && break
        sleep 0.1
    done
    sleep 2
}

wait_for_exit() {
    for _ in $(seq 1 100); do
        if ! kill -0 "$CHROME_PID" 2>/dev/null; then
            wait "$CHROME_PID" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 1. SIGTERM: the session supervisor's logout path. The signal handler turns it
#    into a normal quit, and the log says which signal it was.
start_browser "$RUNTIME/chrome-term.log"
kill -TERM "$CHROME_PID"
if ! wait_for_exit; then
    echo "  FAIL: SIGTERM did not stop the browser"
    tail -40 "$RUNTIME/chrome-term.log"
    exit 1
fi
if ! grep -q 'Ladybird: exiting: received SIGTERM (15)' "$RUNTIME/chrome-term.log"; then
    echo "  FAIL: SIGTERM did not record an exit reason"
    tail -40 "$RUNTIME/chrome-term.log"
    exit 1
fi
echo "  ok: SIGTERM exit reason"

# 2. The Quit action (Alt+F4). This is the deliberate-exit path a user takes;
#    it must not look like a crash or an external signal. The click and the keys
#    share one virtual-input connection: destroying the virtual keyboard in
#    between drops the seat's keyboard focus and the keys would go nowhere.
start_browser "$RUNTIME/chrome-quit.log"
WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 640 400 --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 300 \
    --key 56 press --sleep 100 \
    --key 62 press --sleep 80 \
    --key 62 release --sleep 80 \
    --key 56 release --linger 300 >/dev/null 2>&1
if ! wait_for_exit; then
    echo "  FAIL: the Quit action did not stop the browser"
    tail -40 "$RUNTIME/chrome-quit.log"
    exit 1
fi
if ! grep -q 'Ladybird: exiting: quit action (File > Quit or Alt+F4)' "$RUNTIME/chrome-quit.log"; then
    echo "  FAIL: the Quit action did not record an exit reason"
    tail -40 "$RUNTIME/chrome-quit.log"
    exit 1
fi
echo "  ok: Quit action exit reason"

if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME"/*.log; then
    echo "  FAIL: the browser faulted"
    exit 1
fi
echo "==> exit-reason test PASSED"
