#!/usr/bin/env bash
# Compositor-loss regression test: when the compositor goes away, the frontend
# must record why it ended and save the session, instead of vanishing the way a
# crash does (no exit line, no session update).
#
# Reproduces the 2026-11-11 report: the Wayland connection failed with EPIPE
# ("dispatch failed (errno 32)") and the app left through exit(0) from the
# default LibGUI path, so the session log had no reason and the stored session
# was whatever the last 30s autosave wrote.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="${COMP:-$SDE_ROOT/build/WindowServer/WindowServer}"
CHROME="${LADYBIRD_CHROME:-$ROOT/build/chrome/Ladybird}"

[[ -x "$CHROME" && -x "$COMP" ]] || {
    echo "error: build the frontend and the compositor first" >&2
    exit 1
}

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-compositor-loss.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCK="ladybird-compositor-loss-$$"
PROFILE="compositor-loss-test-$$"
mkdir -p "$RUNTIME/data" "$RUNTIME/config/ladybird-sde" "$RUNTIME/state"

cleanup() {
    kill -9 "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME" "$XDG_RUNTIME_DIR/$SOCK"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    XDG_DATA_HOME="$RUNTIME/data" XDG_CONFIG_HOME="$RUNTIME/config" \
    SDE_STATE_DIR="$RUNTIME/state" SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$LAGOM/lib" \
    "$COMP" -n "$SOCK" >"$RUNTIME/compositor.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 100); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done

env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    XDG_DATA_HOME="$RUNTIME/data" XDG_CONFIG_HOME="$RUNTIME/config" \
    SDE_STATE_DIR="$RUNTIME/state" SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_APP_BIN_DIR="$LAGOM/bin" SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LADYBIRD_SOURCE_DIR="$ROOT" LADYBIRD_SDE_PROFILE="$PROFILE" \
    "$CHROME" about:blank >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

for _ in $(seq 1 250); do grep -q "placed new window 'New Tab - Ladybird'" "$RUNTIME/compositor.log" 2>/dev/null && break; sleep 0.1; done
if ! grep -q "placed new window 'New Tab - Ladybird'" "$RUNTIME/compositor.log" 2>/dev/null; then
    echo "  FAIL: the browser window never appeared"
    tail -20 "$RUNTIME/chrome.log"
    exit 1
fi
sleep 2

# The compositor is the only way this app can paint, so its death ends the
# session. It must not look like a crash.
kill -9 "$COMP_PID" 2>/dev/null || true
COMP_PID=""

STILL_RUNNING=1
for _ in $(seq 1 100); do
    kill -0 "$CHROME_PID" 2>/dev/null || {
        STILL_RUNNING=0
        break
    }
    sleep 0.1
done
if [[ "$STILL_RUNNING" == 1 ]]; then
    echo "  FAIL: the browser is still running after the compositor exited"
    kill -9 "$CHROME_PID" 2>/dev/null || true
    exit 1
fi
set +e
wait "$CHROME_PID"
CHROME_STATUS=$?
set -e
CHROME_PID=""

if [[ "$CHROME_STATUS" != 0 ]]; then
    echo "  FAIL: the browser exited with status $CHROME_STATUS (expected 0)"
    tail -20 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: the browser exits after the compositor disappears"

if ! grep -q "Ladybird: exiting: lost the Wayland connection" "$RUNTIME/chrome.log"; then
    echo "  FAIL: the exit reason does not name the lost compositor connection"
    tail -20 "$RUNTIME/chrome.log"
    exit 1
fi
echo "  ok: the exit reason names the lost Wayland connection"

SESSION="$RUNTIME/config/ladybird-sde/session-$PROFILE.json"
if [[ ! -f "$SESSION" ]]; then
    echo "  FAIL: the session was not saved on the way out"
    tail -20 "$RUNTIME/chrome.log"
    exit 1
fi
python3 - "$SESSION" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
windows = data.get("windows") or []
if len(windows) != 1:
    raise SystemExit(f"expected 1 saved window, got {len(windows)}")
tabs = windows[0].get("tabs") or []
if len(tabs) != 1 or tabs[0].get("url") != "about:blank":
    raise SystemExit(f"unexpected saved tabs: {tabs!r}")
PY
echo "  ok: the session is saved before exiting"

echo "==> compositor-loss test PASSED"
