#!/usr/bin/env python3
# Regression tests for Stage 3 IPC & security hardening:
# - Oversized tab viewport dimensions in create_tab and resize are clamped safely.
# - Dropped file:// payloads exceeding LADYBIRD_SDE_MAX_DROP_FILE_BYTES are skipped safely.
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

PAGE = b"""<!doctype html><html><body style="margin:0;width:800px;height:600px">
<h1>Stage 3 Security</h1>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(PAGE)))
        self.end_headers()
        self.wfile.write(PAGE)


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

    tmp = tempfile.mkdtemp(prefix="ladybird-stage3-test-")
    oversized_drop = os.path.join(tmp, "big.bin")
    with open(oversized_drop, "wb") as f:
        f.write(b"A" * 4096)

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(os.path.join(tmp, "engine.sock"))
    listener.listen(1)

    env = dict(os.environ)
    env["SERENITY_RES_ROOT"] = SERENITY_RES
    env["LADYBIRD_SOURCE_DIR"] = ROOT
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")
    env["XDG_DATA_HOME"] = os.path.join(tmp, "data")
    env["LADYBIRD_SDE_MAX_DROP_FILE_BYTES"] = "1024"
    for key in ("LD_LIBRARY_PATH", "WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(key, None)

    engine_log_path = os.path.join(tmp, "engine.log")
    engine_log = open(engine_log_path, "wb")
    engine = subprocess.Popen(
        [ENGINE, "--engine-socket", os.path.join(tmp, "engine.sock"),
         "--frame-directory", tmp, "--profile", "stage3-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        stream.settimeout(30)
        client = Client(stream)

        client.wait_for(lambda e: e.get("type") == "hello")

        # 1. Oversized create_tab dimensions are clamped to <= 16384.
        url = f"http://127.0.0.1:{port}/"
        client.send({
            "type": "create_tab", "request": 1, "width": 999999, "height": 999999,
            "scale": 1, "url": url, "private": False,
        })
        created = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 1)
        tab_id = created["tab"]
        assert 1 <= created["width"] <= 16384, created
        assert 1 <= created["height"] <= 16384, created
        # Resize back to normal before frame painting so we don't allocate a huge framebuffer.
        client.send({"type": "resize", "tab": tab_id, "width": 800, "height": 600})
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab_id)
        print("ok: oversized create_tab dimensions clamped safely")

        # 2. Oversized dropped file:// is skipped cleanly (engine stays responsive).
        client.send({
            "type": "drag", "tab": tab_id, "drag_type": "drop", "x": 100, "y": 100,
            "urls": [f"file://{oversized_drop}"],
        })
        client.send({"type": "request_processes"})
        procs = client.wait_for(lambda e: e.get("type") == "processes")
        assert len(procs.get("items", [])) > 0, procs
        print("ok: oversized dropped file skipped safely")

        client.send({"type": "quit"})
        engine.wait(timeout=10)

        with open(engine_log_path, "r", errors="replace") as log_file:
            log_text = log_file.read()
        assert "skipped dropped file" in log_text, f"expected drop skip log in:\n{log_text}"
        print("ok: dropped file size cap logged")

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
