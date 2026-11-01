#!/usr/bin/env python3
# Regression test for the engine-facing browser features: User-Agent override,
# navigator compatibility mode, inline-completion eligibility, and page drag
# payloads. Runs the real engine host over its control socket with a local HTTP
# server, so it needs no compositor or frontend window.
#
#   scripts/test-engine-features.py
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.environ.get("LADYBIRD_ENGINE_HOST", os.path.join(ROOT, "Build/release/libexec/LadybirdEngine"))
SERENITY_RES = os.environ.get(
    "SERENITY_RES_ROOT",
    os.path.join(ROOT, "..", "libgui-wayland", "serenity", "Base/res"),
)

LINK_PAGE = b"""<!doctype html><html><body style="margin:0">
<a href="https://example.com/" style="position:absolute;left:10px;top:10px;width:300px;height:40px;background:#ccc">drag me</a>
</body></html>"""

SELECTION_PAGE = b"""<!doctype html><html><body style="margin:0">
<div style="font:32px sans-serif;padding:40px">SELECTIONTEXT</div>
</body></html>"""

NAV_PAGE = b"""<!doctype html><html><body><script>
console.log("NAV vendor=" + navigator.vendor + " appVersion=" + navigator.appVersion + " productSub=" + navigator.productSub + " platform=" + navigator.platform);
</script></body></html>"""

PAGE = b"""<!doctype html><html><body>engine feature test</body></html>"""

requests = []
submissions = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        submissions.append(self.path)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        if self.path == "/api/v1/challenges":
            self.wfile.write(b'{"algorithm":"sha256-seed-nonce-le-v1","token":"test-token","expected_work":1}')
        else:
            self.wfile.write(b'{"receipt":"test-receipt"}')

    def do_GET(self):
        requests.append((self.path, dict(self.headers.items())))
        body = PAGE
        if self.path == "/nav":
            body = NAV_PAGE
        elif self.path == "/drag-link":
            body = LINK_PAGE
        elif self.path == "/drag-select":
            body = SELECTION_PAGE
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Client:
    def __init__(self, stream):
        self.stream = stream
        self.buffer = b""

    def read_line(self, deadline=30):
        end = time.time() + deadline
        while b"\n" not in self.buffer:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("timed out waiting for an engine event")
            chunk = self.stream.recv(65536)
            if not chunk:
                raise EOFError("engine closed the control socket")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def wait_for(self, type_name, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            event = self.read_line(max(0.1, end - time.time()))
            if event.get("type") == type_name:
                return event
        raise TimeoutError(f"timed out waiting for {type_name}")

    def send(self, event):
        self.stream.sendall(json.dumps(event).encode() + b"\n")


def mouse(client, tab, kind, x, y, button="none", buttons=0, clicks=0):
    client.send({
        "type": "mouse", "tab": tab, "event": kind, "x": x, "y": y,
        "button": button, "buttons": buttons, "click_count": clicks,
    })


def create_tab(client, url, request_id):
    client.send({
        "type": "create_tab", "request": request_id, "width": 800, "height": 600,
        "scale": 1, "url": url, "private": False,
    })
    deadline = time.time() + 20
    while time.time() < deadline:
        event = client.read_line(deadline - time.time())
        if event.get("type") == "tab_created" and event.get("request") == request_id:
            return event["tab"]
    raise TimeoutError("no tab_created event")


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="ladybird-engine-test-")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(os.path.join(tmp, "engine.sock"))
    listener.listen(1)

    env = dict(os.environ)
    env["SERENITY_RES_ROOT"] = SERENITY_RES
    env["LADYBIRD_SOURCE_DIR"] = ROOT
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")
    env["XDG_DATA_HOME"] = os.path.join(tmp, "data")
    env["LADYBIRD_REPORTS_ENDPOINT"] = f"http://127.0.0.1:{port}"
    for key in ("LD_LIBRARY_PATH", "WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(key, None)

    crash_report_dir = os.path.join(tmp, "data", "Ladybird", "CrashReports")
    os.makedirs(crash_report_dir)
    crash_report_name = "2026-11-01T00-00-00Z-WebContent-abc123.txt"
    crash_report_text = """Ladybird crash report, format 1
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
"""
    with open(os.path.join(crash_report_dir, crash_report_name), "w") as report:
        report.write(crash_report_text)

    engine_log_path = os.path.join(tmp, "engine.log")
    engine_log = open(engine_log_path, "wb")
    engine = subprocess.Popen(
        [ENGINE, "--engine-socket", os.path.join(tmp, "engine.sock"),
         "--frame-directory", tmp, "--profile", "feature-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        stream.settimeout(30)
        client = Client(stream)

        hello = client.wait_for("hello")
        assert hello.get("protocol") == 17, hello
        user_agents = client.wait_for("user_agents")
        default_user_agent = next(agent["value"] for agent in user_agents["agents"] if agent["name"] == "Default")
        modes = client.wait_for("navigator_compatibility_modes")
        assert modes.get("current") == "chrome", modes
        assert [m["value"] for m in modes.get("modes", [])] == ["chrome", "gecko", "webkit"], modes
        print("ok: protocol 17 and spoofing choices")

        crash = client.wait_for("crash_report_available")
        assert crash.get("name") == crash_report_name, crash
        client.send({"type": "crash_report_open", "name": crash_report_name})
        ready = client.wait_for("crash_report_ready")
        assert ready.get("name") == crash_report_name, ready
        assert any(field.get("label") == "Verification failed" for field in ready.get("fields", [])), ready
        client.send({"type": "crash_report_text"})
        text = client.wait_for("crash_report_text")
        assert "Verification failed" in text.get("text", ""), text
        client.send({"type": "crash_report_send", "description": "engine feature test", "include_website": False})
        sent = client.wait_for("crash_report_sent", timeout=30)
        assert sent.get("type") == "crash_report_sent", sent
        assert "/api/v1/challenges" in submissions and "/api/v1/reports" in submissions, submissions
        print("ok: crash report review and submission")

        client.send({"type": "set_user_agent", "value": "EngineFeatureTest/1.0"})
        changed = client.wait_for("user_agent_changed")
        assert changed.get("value") == "EngineFeatureTest/1.0", changed
        tab = create_tab(client, f"http://127.0.0.1:{port}/ua", 1)
        time.sleep(1)
        assert any(path == "/ua" and headers.get("User-Agent") == "EngineFeatureTest/1.0" for path, headers in requests)
        print("ok: User-Agent override")

        client.send({"type": "request_processes"})
        processes = client.wait_for("processes")
        types = {item.get("type") for item in processes.get("items", [])}
        assert "Browser" in types and "WebContent" in types, types
        print("ok: process manager data")

        client.send({"type": "set_user_agent", "value": default_user_agent})
        client.wait_for("user_agent_changed")
        client.send({"type": "set_navigator_compatibility_mode", "value": "gecko"})
        changed = client.wait_for("navigator_compatibility_mode_changed")
        assert changed.get("value") == "gecko", changed
        tab = create_tab(client, f"http://127.0.0.1:{port}/nav", 2)
        time.sleep(1.5)
        engine_log.flush()
        log_text = open(engine_log_path, errors="replace").read()
        assert "NAV vendor= appVersion=5.0 (X11) productSub=20100101" in log_text, log_text[-2000:]
        print("ok: navigator compatibility mode")

        client.send({"type": "autocomplete", "tab": tab, "request": 7, "query": "lady"})
        items = []
        while True:
            event = client.wait_for("autocomplete_results")
            if event.get("request") != 7:
                continue
            items = event.get("items", [])
            if event.get("final"):
                break
        assert any(item.get("text") == "https://ladybird.org/" and item.get("inline") for item in items), items
        print("ok: inline completion eligibility")

        link_tab = create_tab(client, f"http://127.0.0.1:{port}/drag-link", 3)
        time.sleep(1.5)
        mouse(client, link_tab, "down", 50, 20, "left", 1, 1)
        time.sleep(0.2)
        mouse(client, link_tab, "move", 90, 20, "none", 1)
        drag = client.wait_for("drag_start")
        assert any(item.get("type") == "text/uri-list" and "https://example.com/" in item.get("data", "") for item in drag.get("items", [])), drag
        client.send({"type": "drag", "tab": link_tab, "drag_type": "end", "x": 90, "y": 20})
        print("ok: link drag payload")

        selection_tab = create_tab(client, f"http://127.0.0.1:{port}/drag-select", 4)
        time.sleep(1.5)
        mouse(client, selection_tab, "down", 45, 60, "left", 1, 1)
        time.sleep(0.2)
        mouse(client, selection_tab, "move", 300, 60, "none", 1)
        time.sleep(0.2)
        mouse(client, selection_tab, "up", 300, 60, "left", 0, 1)
        time.sleep(0.4)
        mouse(client, selection_tab, "down", 100, 60, "left", 1, 1)
        time.sleep(0.2)
        mouse(client, selection_tab, "move", 140, 60, "none", 1)
        drag = client.wait_for("drag_start")
        assert any(item.get("type") == "text/plain" and "SELECTIONTEXT" in item.get("data", "") for item in drag.get("items", [])), drag
        client.send({"type": "drag", "tab": selection_tab, "drag_type": "end", "x": 140, "y": 60})
        print("ok: selection drag payload")

        client.send({"type": "quit"})
        time.sleep(0.3)
        stream.close()
        print("PASS")
        return 0
    finally:
        listener.close()
        engine.terminate()
        try:
            engine.wait(timeout=5)
        except subprocess.TimeoutExpired:
            engine.kill()
        engine_log.close()
        server.shutdown()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
