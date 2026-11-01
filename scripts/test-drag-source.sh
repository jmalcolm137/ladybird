#!/usr/bin/env bash
# Frontend drag-source regression test.
#
# Runs the LibGUI browser under the SDE's headless compositor, drags a link in
# the page, and asserts that the compositor accepted a native drag and that the
# drag ended cleanly. It also fails on an assertion or crash in the browser.
#
# The compositor and virtual-input harness belong to the SDE checkout. When they
# are absent this test skips, like the other SDE integration tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"
PORT="${LADYBIRD_DRAG_TEST_PORT:-8769}"

[[ -x "$ROOT/build/chrome/Ladybird" ]] || { echo "error: build the frontend first" >&2; exit 1; }
[[ -x "$ROOT/Build/release/libexec/LadybirdEngine" ]] || { echo "error: build the engine first" >&2; exit 1; }
[[ -x "$COMP" && -x "$VI" ]] || { echo "SKIP: SDE compositor or virtual input is missing"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 is missing"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-drag-test.XXXXXX")"
PAGE_DIR="$RUNTIME/page"
mkdir -p "$PAGE_DIR"
chmod 700 "$RUNTIME"
cat > "$PAGE_DIR/index.html" <<'EOF'
<!doctype html><html><body style="margin:0;background:#ddeeff">
<a href="https://example.com/" style="position:fixed;inset:0;display:block;color:#102030">drag me</a>
</body></html>
EOF

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal"
SOCK="ladybird-drag-test-$$"

cleanup() {
    kill "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

(cd "$PAGE_DIR" && exec python3 -m http.server "$PORT" --bind 127.0.0.1) >"$RUNTIME/http.log" 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.1
done

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$LAGOM/lib" \
    "$COMP" -n "$SOCK" >"$RUNTIME/compositor.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 100); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done

env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    SDE_APP_BIN_DIR="$LAGOM/bin" \
    LADYBIRD_SOURCE_DIR="$ROOT" \
    LADYBIRD_ENGINE_HOST="$ROOT/Build/release/libexec/LadybirdEngine" \
    LADYBIRD_SDE_PROFILE="drag-test-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    "$ROOT/build/chrome/Ladybird" "http://127.0.0.1:$PORT/" >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

mapped=0
for _ in $(seq 1 150); do
    if grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null; then
        mapped=1
        break
    fi
    sleep 0.1
done
if [[ "$mapped" != 1 ]]; then
    echo "error: the browser window did not map" >&2
    tail -30 "$RUNTIME/chrome.log" >&2
    exit 1
fi
sleep 2

env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 400 320 --sleep 200 \
    --button 0x110 press --sleep 400 \
    --move 500 320 --sleep 400 \
    --button 0x110 release --linger 1000 >/dev/null 2>&1 || true
sleep 1

fail=0
if grep -q "drag started" "$RUNTIME/compositor.log"; then
    echo "  ok: the compositor accepted a drag from the browser"
else
    echo "  FAIL: no native drag was started"
    fail=1
fi
if grep -q "drag ended" "$RUNTIME/compositor.log"; then
    echo "  ok: the drag ended cleanly"
else
    echo "  FAIL: the drag did not end"
    fail=1
fi
if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log" "$RUNTIME/compositor.log"; then
    echo "  FAIL: a process faulted"
    fail=1
fi

if [[ "$fail" != 0 ]]; then
    echo "--- compositor ---"
    tail -40 "$RUNTIME/compositor.log"
    echo "--- chrome ---"
    tail -60 "$RUNTIME/chrome.log"
    echo "==> drag-source test FAILED"
    exit 1
fi
echo "==> drag-source test PASSED"
