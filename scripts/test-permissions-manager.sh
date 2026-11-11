#!/usr/bin/env bash
# Permissions manager regression test: stored site decisions are listed, one
# origin can be revoked, and Clear All empties the profile store.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"
CHROME="${LADYBIRD_CHROME:-$ROOT/build/chrome/Ladybird}"

[[ -x "$CHROME" && -x "$COMP" && -x "$VI" ]] || {
    echo "error: build the frontend, compositor and virtual-input tool first" >&2
    exit 1
}

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-permissions.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCK="ladybird-permissions-$$"
PROFILE="permissions-test-$$"
mkdir -p "$RUNTIME/data" "$RUNTIME/config/ladybird-sde" "$RUNTIME/state"

cat > "$RUNTIME/config/ladybird-sde/permissions-$PROFILE.json" <<'EOF'
{"version":1,"origins":{"https://example.com":[{"name":"geolocation","granted":true}],"https://example.org":[{"name":"notifications","granted":false}]}}
EOF

cleanup() {
    kill "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
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
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    "$CHROME" about:blank >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!
for _ in $(seq 1 150); do grep -q "placed new window 'New Tab - Ladybird'" "$RUNTIME/compositor.log" 2>/dev/null && break; sleep 0.1; done
sleep 2

click() {
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$VI" \
        --move "$1" "$2" --sleep 150 --button 0x110 press --sleep 60 --button 0x110 release --sleep 400 \
        >/dev/null 2>&1
}

# Tools -> Permissions… (the item is at the bottom of the Tools menu).
click 378 133
click 500 357
for _ in $(seq 1 100); do grep -q "toplevel mapped title='Permissions'" "$RUNTIME/compositor.log" 2>/dev/null && break; sleep 0.1; done
if ! grep -q "toplevel mapped title='Permissions'" "$RUNTIME/compositor.log"; then
    echo "  FAIL: the Permissions window did not open"
    tail -30 "$RUNTIME/chrome.log"
    exit 1
fi

# Select the first row, Revoke it, then Clear All.
click 300 185
click 155 493
sleep 0.5
python3 - "$RUNTIME/config/ladybird-sde/permissions-$PROFILE.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
origins = d.get("origins", {})
if "https://example.com" in origins:
    raise SystemExit("revoke did not remove example.com")
if "https://example.org" not in origins:
    raise SystemExit("revoke removed the wrong origin")
PY
echo "  ok: revoke removes one origin"

click 250 493
sleep 0.5
python3 - "$RUNTIME/config/ladybird-sde/permissions-$PROFILE.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
if d.get("origins"):
    raise SystemExit("Clear All did not empty the store")
PY
echo "  ok: Clear All empties the store"

echo "==> permissions-manager test PASSED"
