#!/usr/bin/env bash
# Compatibility-report submission regression test.
#
# Runs the LibGUI browser under the SDE headless compositor against a local
# report service, opens Help -> Report Website Issue..., fills the description,
# and asserts that the engine requested a challenge and submitted the report.
#
# The report review window and the local service are deliberately exercised
# end-to-end; the server, not the screenshot, is the oracle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"
PORT="${LADYBIRD_REPORT_TEST_PORT:-8130}"

[[ -x "$ROOT/build/chrome/Ladybird" ]] || { echo "error: build the frontend first" >&2; exit 1; }
[[ -x "$ROOT/Build/release/libexec/LadybirdEngine" ]] || { echo "error: build the engine first" >&2; exit 1; }
[[ -x "$COMP" && -x "$VI" ]] || { echo "SKIP: SDE compositor or virtual input is missing"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 is missing"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-report-test.XXXXXX")"
chmod 700 "$RUNTIME"
cat > "$RUNTIME/server.py" <<'PY'
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import sys

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        print(self.path, flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        if self.path == "/api/v1/challenges":
            self.wfile.write(b'{"algorithm":"sha256-seed-nonce-le-v1","token":"test-token","expected_work":1}')
        else:
            self.wfile.write(b'{"receipt":"test-receipt"}')

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
PY

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data" "$RUNTIME/config"
SOCK="ladybird-report-test-$$"

cleanup() {
    kill "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

python3 "$RUNTIME/server.py" "$PORT" >"$RUNTIME/server.log" 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 50); do
    curl -fsS -X POST "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.1
done
: > "$RUNTIME/server.log"

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
    LADYBIRD_SDE_PROFILE="report-test-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    LADYBIRD_REPORTS_ENDPOINT="http://127.0.0.1:$PORT" \
    "$ROOT/build/chrome/Ladybird" "https://example.com/" >"$RUNTIME/chrome.log" 2>&1 &
CHROME_PID=$!

for _ in $(seq 1 150); do
    grep -q 'toplevel mapped' "$RUNTIME/compositor.log" 2>/dev/null && break
    sleep 0.1
done
sleep 4

click() {
    WAYLAND_DISPLAY="$SOCK" "$VI" --move "$1" "$2" --sleep 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 500 >/dev/null 2>&1
}
type_text() {
    local text="$1"
    local args=()
    local i ch code
    declare -A keys=([a]=30 [b]=48 [c]=46 [d]=32 [e]=18 [f]=33 [g]=34 [h]=35 [i]=23 [j]=36 [k]=37 [l]=38 [m]=50 [n]=49 [o]=24 [p]=25 [q]=16 [r]=19 [s]=31 [t]=20 [u]=22 [v]=47 [w]=17 [x]=45 [y]=21 [z]=44 [1]=2 [2]=3 [3]=4 [4]=5 [5]=6 [6]=7 [7]=8 [8]=9 [9]=10 [0]=11 [.]=52 [/]=53 [-]=12 [\ ]=57)
    for ((i = 0; i < ${#text}; i++)); do
        ch="${text:i:1}"
        code="${keys[$ch]:-}"
        [[ -n "$code" ]] || continue
        args+=(--key "$code" press --sleep 40 --key "$code" release --sleep 40)
    done
    WAYLAND_DISPLAY="$SOCK" "$VI" "${args[@]}" >/dev/null 2>&1
}

# Help -> Report Website Issue..., then describe the problem and send.
click 424 135
click 424 184
for _ in $(seq 1 100); do
    grep -q "title='Report Website Issue'" "$RUNTIME/chrome.log" 2>/dev/null && break
    sleep 0.1
done
sleep 2
click 400 350
type_text "broken page"
sleep 1
click 704 505
sleep 5
if ! (grep -q '^/api/v1/challenges$' "$RUNTIME/server.log" 2>/dev/null && grep -q '^/api/v1/reports$' "$RUNTIME/server.log" 2>/dev/null); then
    # A click during layout can land before the button is ready; give it one more try.
    click 704 505
fi
for _ in $(seq 1 100); do
    if grep -q '^/api/v1/challenges$' "$RUNTIME/server.log" 2>/dev/null && grep -q '^/api/v1/reports$' "$RUNTIME/server.log" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

if grep -q '^/api/v1/challenges$' "$RUNTIME/server.log" && grep -q '^/api/v1/reports$' "$RUNTIME/server.log"; then
    echo "  ok: the report service received a challenge request and a report"
else
    echo "  FAIL: the report service did not receive the complete submission"
    echo "--- server ---"
    cat "$RUNTIME/server.log"
    echo "--- chrome ---"
    tail -60 "$RUNTIME/chrome.log"
    exit 1
fi
if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log" "$RUNTIME/compositor.log"; then
    echo "  FAIL: a process faulted"
    exit 1
fi
echo "==> compatibility-report test PASSED"
