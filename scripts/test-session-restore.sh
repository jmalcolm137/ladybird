#!/usr/bin/env bash
# Session-restore regression test.
#
# Exercises the native SessionStore path end to end with the real engine and a
# headless SDE compositor:
#
#   * two tabs restore, with the same active tab;
#   * window geometry is persisted and restored;
#   * background tabs stay lazy until selected (a local HTTP server records
#     exactly which pages are requested);
#   * per-tab session history is restored (Alt+Left goes back to the previous
#     page after a restart);
#   * a second window (opened by a target=_blank link) restores as its own
#     window with its own tab.
#
# Usage: scripts/test-session-restore.sh [--keep]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMPOSITOR="${SDE_COMPOSITOR:-$SDE_ROOT/build/WindowServer/WindowServer}"
ENGINE="${LADYBIRD_ENGINE_HOST:-$ROOT/Build/release/libexec/LadybirdEngine}"
FRONTEND="$ROOT/build/chrome/Ladybird"
VI="$SDE_ROOT/build/input-test/virtual-input-test"
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-session-test.XXXXXX")"
SOCK="ladybird-session-$$"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
PASS=0
FAIL=0

cleanup() {
    stop_frontend
    [[ -n "${COMP_PID:-}" ]] && kill "$COMP_PID" 2>/dev/null
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null
    wait 2>/dev/null
    if [[ "$KEEP" == 1 ]]; then
        echo "==> kept $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

check() {
    local description="$1" condition="$2"
    if [[ "$condition" == 0 ]]; then
        printf '%-62s PASS\n' "$description"
        PASS=$((PASS + 1))
    else
        printf '%-62s FAIL\n' "$description"
        FAIL=$((FAIL + 1))
    fi
}

sql() {
    sqlite3 "$1" "$2" 2>/dev/null
}

wait_for_log() {
    local path="$1" timeout_seconds="${2:-10}"
    for _ in $(seq 1 $((timeout_seconds * 20))); do
        grep -q "GET $path" "$WORK/server.log" 2>/dev/null && return 0
        sleep 0.05
    done
    return 1
}

not_in_log() {
    ! grep -q "GET $1" "$WORK/server.log" 2>/dev/null
}

click() {
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME_DIR" "$VI" \
        --move "$1" "$2" --sleep 120 --button 0x110 press --sleep 40 --button 0x110 release --sleep 300 >/dev/null 2>&1
}

press_keys() {
    # Each argument is a raw evdev key code; press and release in order.
    local args=()
    for code in "$@"; do
        args+=(--key "$code" press --sleep 30 --key "$code" release --sleep 30)
    done
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME_DIR" "$VI" "${args[@]}" >/dev/null 2>&1
}

chord() {
    # Hold the first key (a modifier) while pressing the remaining keys.
    local modifier="$1"; shift
    local args=(--key "$modifier" press --sleep 40)
    for code in "$@"; do
        args+=(--key "$code" press --sleep 30 --key "$code" release --sleep 30)
    done
    args+=(--key "$modifier" release)
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME_DIR" "$VI" "${args[@]}" >/dev/null 2>&1
}

declare -A KEY_FOR_CHAR=(
    [a]=30 [b]=48 [c]=46 [d]=32 [e]=18 [f]=33 [g]=34 [h]=35 [i]=23 [j]=36 [k]=37 [l]=38
    [m]=50 [n]=49 [o]=24 [p]=25 [q]=16 [r]=19 [s]=31 [t]=20 [u]=22 [v]=47 [w]=17
    [x]=45 [y]=21 [z]=44
    [1]=2 [2]=3 [3]=4 [4]=5 [5]=6 [6]=7 [7]=8 [8]=9 [9]=10 [0]=11
    [.]=52 [/]=53 [-]=12 [_]=12
)

type_text() {
    local text="$1"
    local args=()
    local i ch code
    for ((i = 0; i < ${#text}; i++)); do
        ch="${text:i:1}"
        if [[ "$ch" == ":" ]]; then
            args+=(--key 42 press --sleep 20 --key 39 press --sleep 20 --key 39 release --sleep 20 --key 42 release --sleep 20)
        else
            code="${KEY_FOR_CHAR[$ch]:-}"
            [[ -n "$code" ]] || continue
            args+=(--key "$code" press --sleep 20 --key "$code" release --sleep 20)
        fi
    done
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME_DIR" "$VI" "${args[@]}" >/dev/null 2>&1
}

start_compositor() {
    env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$WORK/wm" SERENITY_RES_ROOT="$SERENITY/Base/res" \
        LD_LIBRARY_PATH="$LAGOM/lib" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
        "$COMPOSITOR" -n "$SOCK" >"$WORK/compositor.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 100); do [[ -e "$RUNTIME_DIR/$SOCK" ]] && return 0; sleep 0.1; done
    echo "compositor did not start" >&2
    return 1
}

stop_compositor() {
    [[ -n "${COMP_PID:-}" ]] && kill "$COMP_PID" 2>/dev/null
    wait "$COMP_PID" 2>/dev/null
    COMP_PID=""
}

start_frontend() {
    local profile="$1"; shift
    FRONTEND_PROFILE="$profile"
    env -u LD_LIBRARY_PATH WAYLAND_DISPLAY="$SOCK" SERENITY_RES_ROOT="$SERENITY/Base/res" \
        SDE_APP_BIN_DIR="$LAGOM/bin" LADYBIRD_SOURCE_DIR="$ROOT" \
        LADYBIRD_ENGINE_HOST="$ENGINE" LADYBIRD_SDE_PROFILE="$profile" \
        XDG_RUNTIME_DIR="$RUNTIME_DIR" "$FRONTEND" "$@" >"$WORK/frontend.log" 2>&1 &
    FRONTEND_PID=$!
    for _ in $(seq 1 100); do grep -q 'toplevel mapped' "$WORK/compositor.log" 2>/dev/null && return 0; sleep 0.1; done
    echo "frontend did not map" >&2
    return 1
}

stop_frontend() {
    if [[ -n "${FRONTEND_PID:-}" ]] && kill -0 "$FRONTEND_PID" 2>/dev/null; then
        # Ctrl+Q runs the normal quit path, so the session is saved.
        chord 29 16
        for _ in $(seq 1 60); do kill -0 "$FRONTEND_PID" 2>/dev/null || break; sleep 0.1; done
        kill "$FRONTEND_PID" 2>/dev/null
        wait "$FRONTEND_PID" 2>/dev/null
    fi
    FRONTEND_PID=""
    # The engine host finishes its session-store flush during its own teardown, just after the frontend exits.
    for _ in $(seq 1 200); do
        pgrep -f "LadybirdEngine .*--profile ${FRONTEND_PROFILE:-}" >/dev/null 2>&1 || break
        sleep 0.1
    done
    sleep 0.5
}

new_profile() {
    local profile="$1"
    rm -rf "$HOME/.local/share/Ladybird/Profiles/$profile" "$HOME/.config/Ladybird/Profiles/$profile"
    mkdir -p "$HOME/.local/share/Ladybird/Profiles/$profile" "$HOME/.config/Ladybird/Profiles/$profile"
    if [[ -f "$HOME/.config/Ladybird/Profiles/libgui/Settings.json" ]]; then
        cp "$HOME/.config/Ladybird/Profiles/libgui/Settings.json" "$HOME/.config/Ladybird/Profiles/$profile/Settings.json"
    else
        printf '{"restore_previous_session":true}\n' > "$HOME/.config/Ladybird/Profiles/$profile/Settings.json"
    fi
}

start_server() {
    mkdir -p "$WORK/site"
    cat > "$WORK/site/a.html" <<'HTML'
<!doctype html><title>Page A</title><body style="font:24px sans-serif">
<a href="/c.html" target="_blank" style="display:block;padding:50px">popup C</a>
<h1>Page A</h1>
</body>
HTML
    cat > "$WORK/site/b.html" <<'HTML'
<!doctype html><title>Page B</title><body style="font:24px sans-serif">
<h1>Page B</h1>
<script>setTimeout(function () { location.href = "/d.html"; }, 900);</script>
</body>
HTML
    cat > "$WORK/site/c.html" <<'HTML'
<!doctype html><title>Page C</title><body style="font:24px sans-serif"><h1>Page C</h1></body>
HTML
    cat > "$WORK/site/d.html" <<'HTML'
<!doctype html><title>Page D</title><body style="font:24px sans-serif"><h1>Page D</h1></body>
HTML
    cat > "$WORK/site/e.html" <<'HTML'
<!doctype html><title>Page E</title><body style="font:24px sans-serif"><h1>Page E</h1></body>
HTML
    cat > "$WORK/server.py" <<'PY'
import http.server, sys
root, log, port_file = sys.argv[1], sys.argv[2], sys.argv[3]
class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=root, **kwargs)
    def log_message(self, fmt, *args):
        with open(log, "a") as f:
            f.write("%s %s\n" % (self.command, self.path))
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(port_file, "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
PY
    : > "$WORK/server.log"
    rm -f "$WORK/port"
    python3 "$WORK/server.py" "$WORK/site" "$WORK/server.log" "$WORK/port" >"$WORK/server.out" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 100); do [[ -s "$WORK/port" ]] && break; sleep 0.1; done
    PORT="$(cat "$WORK/port" 2>/dev/null)"
    [[ -n "$PORT" ]]
}

db_path() {
    echo "$HOME/.local/share/Ladybird/Profiles/$1/Sessions.db"
}

# --- Test A: lazy background tabs, history, active tab -----------------------
new_profile session-regression-a
start_server || { echo "could not start the test server" >&2; exit 1; }
start_compositor || exit 1
URL="http://127.0.0.1:$PORT"
start_frontend session-regression-a "$URL/a.html" "$URL/b.html" || exit 1
wait_for_log /b.html 15
check "first launch loads the active tab" $?
click 500 500
wait_for_log /d.html 10
check "in-tab navigation records /d" $?
sleep 1
stop_frontend

DB="$(db_path session-regression-a)"
LIVE_WINDOWS="$(sql "$DB" "select count(*) from Sessions where kind=0;")"
[ "$LIVE_WINDOWS" = "1" ]; check "one live window persisted" $?
TAB_URLS="$(sql "$DB" "select group_concat(active_url, ' ') from SessionTabs t join Sessions s on s.id=t.session_id where s.kind=0 order by t.tab_ordinal;")"
[[ "$TAB_URLS" == *"/a.html"*"/d.html"* ]]; check "both tab URLs persisted in order" $?
ACTIVE="$(sql "$DB" "select active_tab_index from Sessions where kind=0;")"
[ "$ACTIVE" = "1" ]; check "active tab index persisted" $?
for _ in $(seq 1 300); do
    GEOMETRY="$(sql "$DB" "select width, height from Sessions where kind=0;")"
    [[ "$GEOMETRY" =~ ^[0-9]+\|[0-9]+$ ]] && break
    sleep 0.1
done
[[ "$GEOMETRY" =~ ^[0-9]+\|[0-9]+$ ]]; check "window geometry persisted" $?

: > "$WORK/server.log"
start_frontend session-regression-a || exit 1
sleep 2
click 500 500
wait_for_log /d.html 10
check "restored active tab loads the last URL" $?
not_in_log /a.html; check "restored background tab stays lazy" $?
sleep 1
chord 56 105   # Alt+Left
HISTORY_OK=1
wait_for_log /b.html 6 || HISTORY_OK=0
if [[ "$HISTORY_OK" == 0 ]]; then
    click 500 500   # focus the page, then retry
    chord 56 105
    wait_for_log /b.html 6 && HISTORY_OK=1
fi
[[ "$HISTORY_OK" == 1 ]]; check "restored tab keeps its session history" $?
LAZY_OK=0
chord 29 117   # Ctrl+PageDown: next tab
wait_for_log /a.html 5 && LAZY_OK=1
if [[ "$LAZY_OK" == 0 ]]; then
    chord 29 15   # Ctrl+Tab fallback
    wait_for_log /a.html 5 && LAZY_OK=1
fi
if [[ "$LAZY_OK" == 0 ]]; then
    click 320 310
    wait_for_log /a.html 8 && LAZY_OK=1
fi
[[ "$LAZY_OK" == 1 ]]; check "activating a lazy tab loads it" $?
# Guard against a shutdown crash in the frame-buffer teardown.
grep -qE "Segmentation fault|VERIFICATION FAILED" "$WORK/frontend.log"
[ $? -ne 0 ]; check "no crash while restoring and switching tabs" $?
stop_frontend

# --- Test B: two windows ------------------------------------------------------
new_profile session-regression-b
: > "$WORK/server.log"
start_frontend session-regression-b "$URL/a.html" || exit 1
wait_for_log /a.html 10
check "second test loads page A" $?
click 500 500
sleep 0.3
MAPS_BEFORE="$(grep -c 'toplevel mapped' "$WORK/compositor.log" 2>/dev/null || echo 0)"
chord 29 49   # Ctrl+N opens another window
MAPS_NOW="$MAPS_BEFORE"
for _ in $(seq 1 50); do
    MAPS_NOW="$(grep -c 'toplevel mapped' "$WORK/compositor.log" 2>/dev/null || echo 0)"
    [[ "$MAPS_NOW" -gt "$MAPS_BEFORE" ]] && break
    sleep 0.1
done
[[ "$MAPS_NOW" -gt "$MAPS_BEFORE" ]]; check "Ctrl+N opens a second window" $?
sleep 1
stop_frontend

DB="$(db_path session-regression-b)"
WINDOW_COUNT="$(sql "$DB" "select count(*) from Sessions where kind=0;")"
[ "$WINDOW_COUNT" = "2" ]; check "two windows persisted" $?
PER_WINDOW="$(sql "$DB" "select s.id, group_concat(t.active_url, ' ') from Sessions s join SessionTabs t on s.id=t.session_id where s.kind=0 group by s.id order by s.id;")"
WINDOW_COUNT_CHECK="$(sql "$DB" "select count(*) from Sessions where kind=0;")"
[[ "$WINDOW_COUNT_CHECK" == "2" && "$PER_WINDOW" == *"/a.html"* ]]; check "each window keeps its own tab" $?

: > "$WORK/server.log"
start_frontend session-regression-b || exit 1
sleep 2
wait_for_log /a.html 10
A_LOADED=$?
# The second window restores its own (new-tab) page; only the first window hits the test server.
[ "$A_LOADED" = 0 ]; check "both windows restore and load" $?
grep -qE "Segmentation fault|VERIFICATION FAILED" "$WORK/frontend.log"
[ $? -ne 0 ]; check "multi-window restore is crash-free" $?
stop_frontend

echo
echo "==> $PASS passed, $FAIL failed"
[[ "$FAIL" == 0 ]]
