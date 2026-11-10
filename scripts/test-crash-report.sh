#!/usr/bin/env bash
# Crash-report submission UI regression test.
#
# Places a synthetic crash report in a disposable profile, opens the crash
# notification and review window under the SDE headless compositor, sends the
# report to a local service, and asserts the service received the challenge and
# the report.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE_ROOT="${SDE_ROOT:-$ROOT/../serenity-desktop-environment}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$SDE_ROOT/build/input-test/virtual-input-test"
PORT="${LADYBIRD_CRASH_TEST_PORT:-8131}"

[[ -x "$ROOT/build/chrome/Ladybird" && -x "$ROOT/Build/release/libexec/LadybirdEngine" ]] || {
    echo "error: build the frontend and engine first" >&2
    exit 1
}
[[ -x "$COMP" && -x "$VI" ]] || { echo "SKIP: SDE compositor or virtual input is missing"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-crash-test.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SDE_STATE_DIR="$RUNTIME"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal" "$RUNTIME/data/Ladybird/CrashReports" "$RUNTIME/config"

cat > "$RUNTIME/data/Ladybird/CrashReports/2026-11-01T00-00-00Z-WebContent-abc123.txt" <<'EOF'
Ladybird crash report, format 1
Process: WebContent
Crashed at: 2026-11-01T00:00:00Z
Version: 1.0
Platform: Linux
Architecture: x86_64
Git commit: 5ebbe272
Verification failed: value != 0 at Services/WebContent/Foo.cpp:123
Termination signal: SIGTRAP
Termination signal number: 5

Native stack (binary build ID, object address):
#0 abcdef 0x1234 Web::Page::crash()
EOF

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

python3 "$RUNTIME/server.py" "$PORT" >"$RUNTIME/server.log" 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 50); do
    curl -fsS -X POST "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.1
done
: > "$RUNTIME/server.log"

SOCK="ladybird-crash-test-$$"
cleanup() {
    kill "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
    wait "${HTTP_PID:-}" "${CHROME_PID:-}" "${COMP_PID:-}" 2>/dev/null || true
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
    LADYBIRD_SDE_PROFILE="crash-test-$$" \
    LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1 \
    LADYBIRD_REPORTS_ENDPOINT="http://127.0.0.1:$PORT" \
    "$ROOT/build/chrome/Ladybird" about:newtab >"$RUNTIME/chrome.log" 2>&1 &
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
        args+=(--key "$code" press --sleep 25 --key "$code" release --sleep 25)
    done
    WAYLAND_DISPLAY="$SOCK" "$VI" "${args[@]}" >/dev/null 2>&1
}

# Review report -> describe the crash -> send.
click 520 287
sleep 1
click 400 570
type_text "test crash"
click 812 702
sleep 6

if grep -q '^/api/v1/challenges$' "$RUNTIME/server.log" && grep -q '^/api/v1/reports$' "$RUNTIME/server.log"; then
    echo "  ok: the crash report was submitted"
else
    echo "  FAIL: the crash report service did not receive the submission"
    echo "--- server ---"; cat "$RUNTIME/server.log"
    echo "--- chrome ---"; tail -60 "$RUNTIME/chrome.log"
    exit 1
fi
if grep -qiE "VERIFICATION FAILED|ASSERTION FAILED|Segmentation fault|SIGSEGV" "$RUNTIME/chrome.log" "$RUNTIME/compositor.log"; then
    echo "  FAIL: a process faulted"
    exit 1
fi
echo "==> crash-report test PASSED"
