#!/usr/bin/env python3
# Regression tests for Stage 1 safety & bug fixes:
# - Child tabs created via window.open() use EngineHeadlessWebView (no invalid downcast UB)
#   and emit context menus cleanly.
# - Closing a tab while a JS dialog is pending tears down cleanly.
# - Private tab certificate_info requests do not crash or leak.
# - Process snapshots include the "type" field for Browser protection.
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

OPENER_PAGE = b"""<!doctype html><html><body>
<button id="btn" style="position:absolute;left:0;top:0;width:400px;height:400px"
  onclick="window.open('/child', '_blank')">open</button>
</body></html>"""

CHILD_PAGE = b"""<!doctype html><html><body style="margin:0;width:800px;height:600px">
<div style="width:400px;height:400px">child tab content</div>
</body></html>"""

ALERT_PAGE = b"""<!doctype html><html><body><script>
setTimeout(function() { alert("stage1-test-alert"); }, 50);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body = CHILD_PAGE
        if self.path == "/opener":
            body = OPENER_PAGE
        elif self.path == "/alert":
            body = ALERT_PAGE
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

    def wait_for(self, predicate, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            event = self.read_line(max(0.1, end - time.time()))
            if predicate(event):
                return event
        raise TimeoutError("timed out waiting for expected event")

    def send(self, event):
        self.stream.sendall(json.dumps(event).encode() + b"\n")


def main():
    if not os.path.exists(ENGINE):
        print(f"error: engine host not found at {ENGINE}", file=sys.stderr)
        return 1

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    tmp = tempfile.mkdtemp(prefix="ladybird-stage1-test-")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(os.path.join(tmp, "engine.sock"))
    listener.listen(1)

    env = dict(os.environ)
    env["SERENITY_RES_ROOT"] = SERENITY_RES
    env["LADYBIRD_SOURCE_DIR"] = ROOT
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")
    env["XDG_DATA_HOME"] = os.path.join(tmp, "data")
    for key in ("LD_LIBRARY_PATH", "WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(key, None)

    engine_log_path = os.path.join(tmp, "engine.log")
    engine_log = open(engine_log_path, "wb")
    engine = subprocess.Popen(
        [ENGINE, "--engine-socket", os.path.join(tmp, "engine.sock"),
         "--frame-directory", tmp, "--profile", "stage1-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        stream.settimeout(30)
        client = Client(stream)

        client.wait_for(lambda e: e.get("type") == "hello")

        # 1. Child tab created via window.open() must be an EngineHeadlessWebView and support context menus.
        client.send({
            "type": "create_tab", "request": 1, "width": 800, "height": 600,
            "scale": 1, "url": f"http://127.0.0.1:{port}/opener", "private": False,
        })
        opener = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 1)
        opener_id = opener["tab"]
        client.wait_for(lambda e: e.get("type") == "url" and e.get("tab") == opener_id and "/opener" in e.get("url", ""))
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == opener_id)
        client.send({"type": "focus", "tab": opener_id, "focused": True})
        time.sleep(0.3)

        client.send({"type": "mouse", "tab": opener_id, "event": "down", "x": 100, "y": 100, "button": "left", "buttons": 1, "click_count": 1})
        client.send({"type": "mouse", "tab": opener_id, "event": "up", "x": 100, "y": 100, "button": "left", "buttons": 0, "click_count": 1})

        child = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("parent") == opener_id)
        child_id = child["tab"]
        client.wait_for(lambda e: e.get("type") == "url" and e.get("tab") == child_id and "/child" in e.get("url", ""))
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == child_id)
        client.send({"type": "focus", "tab": child_id, "focused": True})
        time.sleep(0.3)

        # Right-click in the child tab to verify EngineHeadlessWebView::did_request_context_menu works without UB.
        client.send({"type": "mouse", "tab": child_id, "event": "down", "x": 100, "y": 100, "button": "right", "buttons": 2, "click_count": 1})
        client.send({"type": "mouse", "tab": child_id, "event": "up", "x": 100, "y": 100, "button": "right", "buttons": 0, "click_count": 1})
        menu = client.wait_for(lambda e: e.get("type") == "context_menu" and e.get("tab") == child_id)
        assert len(menu.get("items", [])) > 0, menu
        print("ok: child tab EngineHeadlessWebView and context menu")

        # 2. Close a tab while a JS alert dialog is open.
        client.send({
            "type": "create_tab", "request": 2, "width": 800, "height": 600,
            "scale": 1, "url": f"http://127.0.0.1:{port}/alert", "private": False,
        })
        alert_tab = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 2)
        alert_tab_id = alert_tab["tab"]
        dialog = client.wait_for(lambda e: e.get("type") == "dialog" and e.get("tab") == alert_tab_id)
        assert dialog.get("kind") == "alert", dialog
        client.send({"type": "close_tab", "tab": alert_tab_id})
        client.wait_for(lambda e: e.get("type") == "tab_closed" and e.get("tab") == alert_tab_id)
        print("ok: closing tab with pending dialog")

        # 3. Private tab certificate_info error path.
        client.send({
            "type": "create_tab", "request": 3, "width": 800, "height": 600,
            "scale": 1, "url": f"http://127.0.0.1:{port}/child", "private": True,
        })
        priv_tab = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 3)
        priv_id = priv_tab["tab"]
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == priv_id)
        client.send({"type": "certificate_info", "tab": priv_id})
        cert = client.wait_for(lambda e: e.get("type") == "certificate_info" and e.get("tab") == priv_id)
        assert cert.get("ok") is False, cert
        print("ok: private tab certificate_info error handling")

        # 4. Process manager items include "type" for Browser row.
        client.send({"type": "request_processes"})
        procs = client.wait_for(lambda e: e.get("type") == "processes")
        assert any(p.get("type") == "Browser" for p in procs.get("items", [])), procs
        print("ok: process manager Browser type field")

        client.send({"type": "quit"})
        engine.wait(timeout=10)
        print("PASS")
        return 0
    finally:
        if engine.poll() is None:
            engine.terminate()
            try:
                engine.wait(timeout=5)
            except subprocess.TimeoutExpired:
                engine.kill()
        engine_log.close()
        listener.close()
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
