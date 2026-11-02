#!/usr/bin/env bash
# Address-bar inline-completion regression test.
#
# Types "lady" into the location bar with a fresh profile (whose default
# bookmarks include ladybird.org), asserts that the frontend applied the
# inline completion, then deletes it with Backspace and asserts it does not
# immediately grow back.
#
# The frontend logs applied completions only when
# LADYBIRD_SDE_TEST_LOG_INLINE_COMPLETION is set; that keeps the test
# deterministic without exposing the completion in the UI protocol.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"

[[ -x "$ROOT/build/chrome/Ladybird" ]] || { echo "error: build the frontend first" >&2; exit 1; }
[[ -x "$ROOT/Build/release/libexec/LadybirdEngine" ]] || { echo "error: build the engine first" >&2; exit 1; }
[[ -x "$COMP" && -x "$VI" ]] || { echo "SKIP: SDE compositor or virtual input is missing"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-inline-test.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data" "$RUNTIME/config"
SOCK="ladybird-inline-test-$$"

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
    LADYBIRD_SDE_PROFILE="inline-test-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    LADYBIRD_SDE_TEST_LOG_INLINE_COMPLETION=1 \
    "$ROOT/build/chrome/Ladybird" about:newtab >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

for _ in $(seq 1 150); do
    grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null && break
    sleep 0.1
done
# The window maps before the renderer has prepared the location bar and its
# default bookmarks; give it the extra time so the clicks below are reliable.
sleep 3

# Click the location bar and clear its about:newtab text, then type "lady".
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 700 181 --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 300 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 25 \
    --key 14 press --sleep 25 --key 14 release --sleep 200 \
    --key 38 press --sleep 30 --key 38 release --sleep 60 \
    --key 30 press --sleep 30 --key 30 release --sleep 60 \
    --key 32 press --sleep 30 --key 32 release --sleep 60 \
    --key 21 press --sleep 30 --key 21 release --sleep 2000 >/dev/null 2>&1 || true

if ! grep -q 'inline completion: ladybird.org/' "$RUNTIME/chrome.log"; then
    echo "  FAIL: no ladybird.org inline completion was applied"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: ladybird.org inline completion applied"

before="$(grep -c 'inline completion: ladybird.org/' "$RUNTIME/chrome.log" || true)"
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --key 14 press --sleep 30 --key 14 release --sleep 1500 >/dev/null 2>&1 || true
after="$(grep -c 'inline completion: ladybird.org/' "$RUNTIME/chrome.log" || true)"
if [[ "$after" != "$before" ]]; then
    echo "  FAIL: Backspace did not suppress the completion (before=$before after=$after)"
    tail -40 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: Backspace suppresses the completion"

if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log"; then
    echo "  FAIL: the browser faulted"
    exit 1
fi
echo "==> inline-completion test PASSED"
