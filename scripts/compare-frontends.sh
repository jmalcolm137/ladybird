#!/usr/bin/env bash
# Run the same URL through the upstream Qt frontend and the LibGUI frontend
# headlessly, and fail if either run reports a verification failure. This is a
# crash/regression smoke test, not a pixel comparison: the chrome differs.
#
#   scripts/compare-frontends.sh [url]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URL="${1:-https://example.com/}"
OUT_DIR="${OUT_DIR:-$ROOT/build/frontend-compare}"
QT_BIN="${QT_BIN:-$ROOT/Build/release/bin/Ladybird}"
QT_OUT="$OUT_DIR/qt.png"
LIBGUI_OUT="$OUT_DIR/libgui.png"
QT_LOG="$OUT_DIR/qt.log"
LIBGUI_LOG="$OUT_DIR/libgui.log"

mkdir -p "$OUT_DIR"

if [[ ! -x "$QT_BIN" ]]; then
    echo "compare-frontends: missing Qt frontend at $QT_BIN" >&2
    echo "  build it with: ninja -C Build/release bin/Ladybird" >&2
    exit 1
fi
if [[ ! -x "$ROOT/build/chrome/Ladybird" ]]; then
    echo "compare-frontends: missing LibGUI frontend; run scripts/build-chrome.sh" >&2
    exit 1
fi

echo "==> Qt: $URL"
env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-sde}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
    "$QT_BIN" --headless --screenshot-delay 3 --screenshot-path "$QT_OUT" \
    --window-width 1280 --window-height 800 "$URL" >"$QT_LOG" 2>&1 || true

echo "==> LibGUI: $URL"
LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
LADYBIRD_ENGINE_HOST="$ROOT/Build/release/libexec/LadybirdEngine" \
LADYBIRD_TEST_URL="$URL" \
    "$ROOT/scripts/run-headless-test.sh" "$LIBGUI_OUT" >"$LIBGUI_LOG" 2>&1 || true

failed=0
for log in "$QT_LOG" "$LIBGUI_LOG"; do
    if grep -qE 'VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|Aborted' "$log"; then
        echo "compare-frontends: crash signature in $log" >&2
        grep -nE 'VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|Aborted' "$log" | tail -5 >&2
        failed=1
    fi
done

if [[ ! -s "$QT_OUT" ]]; then
    echo "compare-frontends: Qt produced no screenshot" >&2
    failed=1
fi
if [[ ! -s "$LIBGUI_OUT" ]]; then
    echo "compare-frontends: LibGUI produced no screenshot" >&2
    failed=1
fi

if [[ "$failed" != 0 ]]; then
    echo "compare-frontends: FAIL (logs in $OUT_DIR)" >&2
    exit 1
fi

echo "==> compare-frontends: OK"
echo "    $QT_OUT"
echo "    $LIBGUI_OUT"
