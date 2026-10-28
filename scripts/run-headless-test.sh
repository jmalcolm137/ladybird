#!/usr/bin/env bash
# Smoke-test the LibGUI frontend without a Ladybird build: the fake engine
# publishes a synthetic frame through the real protocol and shared memory, and
# the SDE's headless compositor hosts the real LibGUI window.
#
#   scripts/run-headless-test.sh [output.png]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SOCK="ladybird-sde-$$"
OUTPUT="${1:-$ROOT/build/ladybird-sde-headless.png}"
ENGINE="${LADYBIRD_ENGINE_HOST:-$ROOT/Tools/fake-engine.py}"
TEST_URL="${LADYBIRD_TEST_URL:-}"

[[ -x "$ROOT/build/chrome/Ladybird" ]] || { echo "error: build the frontend first" >&2; exit 1; }
[[ -x "$COMP" ]] || { echo "error: build the SDE WindowServer first" >&2; exit 1; }
command -v python3 >/dev/null || { echo "error: python3 is required" >&2; exit 1; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-sde.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal"

cleanup() {
    kill "${CHROME_PID:-}" 2>/dev/null || true
    kill "${COMP_PID:-}" 2>/dev/null || true
    wait "${CHROME_PID:-}" 2>/dev/null || true
    wait "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$LAGOM/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/compositor.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 100); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
[[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] || { echo "error: compositor did not start" >&2; tail -20 "$RUNTIME/compositor.log" >&2; exit 1; }

CHROME_ARGS=()
[[ -n "$TEST_URL" ]] && CHROME_ARGS+=("$TEST_URL")

env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    SDE_APP_BIN_DIR="$LAGOM/bin" \
    LADYBIRD_SOURCE_DIR="$ROOT" \
    LADYBIRD_ENGINE_HOST="$ENGINE" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    "$ROOT/build/chrome/Ladybird" "${CHROME_ARGS[@]}" > "$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

mapped=0
for _ in $(seq 1 100); do
    if grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null; then
        mapped=1
        break
    fi
    sleep 0.1
done
if [[ "$mapped" != 1 ]]; then
    echo "error: the LibGUI window did not map" >&2
    echo "--- chrome log ---" >&2
    tail -30 "$RUNTIME/chrome.log" >&2 || true
    echo "--- compositor log ---" >&2
    tail -30 "$RUNTIME/compositor.log" >&2 || true
    exit 1
fi

sleep 2

# Exercise the LibGUI -> engine input path when the SDE's virtual input harness
# is available. The fake engine logs every command it receives.
VI="$SDE_ROOT/build/input-test/virtual-input-test"
if [[ -x "$VI" ]]; then
    WAYLAND_DISPLAY="$SOCK" "$VI" \
        --move 400 320 --button 0x110 press --sleep 40 --button 0x110 release --sleep 100 \
        --key 30 press --sleep 40 --key 30 release --sleep 150 \
        >/dev/null 2>&1 || true
    # The test double logs every command it receives; the real engine does not.
    # Assert the input path only when the double is in use.
    if [[ "$(basename "$ENGINE")" == "fake-engine.py" ]]; then
        if ! grep -q '"type":"mouse"' "$RUNTIME/chrome.log"; then
            echo "error: a click did not reach the engine" >&2
            tail -20 "$RUNTIME/chrome.log" >&2
            exit 1
        fi
        if ! grep -q '"type":"key"' "$RUNTIME/chrome.log"; then
            echo "error: a key press did not reach the engine" >&2
            tail -20 "$RUNTIME/chrome.log" >&2
            exit 1
        fi
    fi
fi

if [[ ! -x "$SDE_ROOT/build/tools/sde-screenshot" ]]; then
    "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
fi

PPM="$RUNTIME/frame.ppm"
WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/sde-screenshot" "$PPM"
if command -v magick >/dev/null; then
    magick "$PPM" "$OUTPUT"
elif command -v pnmtopng >/dev/null; then
    pnmtopng "$PPM" > "$OUTPUT"
else
    cp "$PPM" "$OUTPUT"
fi

echo "==> wrote $OUTPUT"
