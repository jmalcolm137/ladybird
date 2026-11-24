#!/usr/bin/env python3
# Regression tests for Stage 2 performance & memory improvements:
# - Frame ring uses 2 slots (33% SHM reduction).
# - Hidden tab SHM frame buffers are released after idle timeout and recreated on re-activation.
# - Autocomplete bookmark cache invalidates cleanly when bookmarks are added/removed.
# - Offset-cursor IPC framing handles burst commands cleanly without message loss.
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

PAGE = b"""<!doctype html><html><head><title>Stage2PerfBookmarkTarget</title></head>
<body style="margin:0;width:800px;height:600px;background:#224466">
<h1>Stage 2 Perf</h1>
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

    tmp = tempfile.mkdtemp(prefix="ladybird-stage2-test-")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(os.path.join(tmp, "engine.sock"))
    listener.listen(1)

    env = dict(os.environ)
    env["SERENITY_RES_ROOT"] = SERENITY_RES
    env["LADYBIRD_SOURCE_DIR"] = ROOT
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")
    env["XDG_DATA_HOME"] = os.path.join(tmp, "data")
    env["LADYBIRD_SDE_HIDDEN_SHM_RELEASE_MS"] = "300"
    for key in ("LD_LIBRARY_PATH", "WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(key, None)

    engine_log_path = os.path.join(tmp, "engine.log")
    engine_log = open(engine_log_path, "wb")
    engine = subprocess.Popen(
        [ENGINE, "--engine-socket", os.path.join(tmp, "engine.sock"),
         "--frame-directory", tmp, "--profile", "stage2-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        stream.settimeout(30)
        client = Client(stream)

        client.wait_for(lambda e: e.get("type") == "hello")

        # 1. Verify 2-slot SHM frame buffer.
        url = f"http://127.0.0.1:{port}/stage2"
        client.send({
            "type": "create_tab", "request": 1, "width": 800, "height": 600,
            "scale": 1, "url": url, "private": False,
        })
        created = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 1)
        tab_id = created["tab"]
        fb = client.wait_for(lambda e: e.get("type") == "frame_buffer" and e.get("tab") == tab_id)
        assert fb.get("slots") == 2, f"expected 2 SHM slots, got {fb}"
        frame = client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab_id)
        client.send({"type": "frame_ack", "tab": tab_id, "sequence": frame["sequence"]})
        print("ok: 2-slot SHM frame ring")

        # 2. Create a second tab and activate it -> tab 1 becomes hidden, its SHM buffer should be
        #    released after idle delay, and recreated when tab 1 is activated again.
        client.send({
            "type": "create_tab", "request": 2, "width": 800, "height": 600,
            "scale": 1, "url": url, "private": False,
        })
        created2 = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 2)
        tab2_id = created2["tab"]
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab2_id)

        client.send({"type": "activate", "tab": tab2_id})
        client.wait_for(lambda e: e.get("type") == "frame_buffer_release" and e.get("tab") == tab_id, timeout=10)
        print("ok: hidden tab SHM frame_buffer_release")

        client.send({"type": "activate", "tab": tab_id})
        fb2 = client.wait_for(lambda e: e.get("type") == "frame_buffer" and e.get("tab") == tab_id, timeout=10)
        assert fb2.get("slots") == 2, fb2
        frame2 = client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab_id, timeout=10)
        client.send({"type": "frame_ack", "tab": tab_id, "sequence": frame2["sequence"]})
        print("ok: SHM frame buffer recreated on re-activation")

        # 3. Autocomplete bookmark cache invalidates when toggling bookmarks.
        client.send({"type": "toggle_bookmark", "tab": tab_id})
        client.wait_for(lambda e: e.get("type") == "bookmarks")

        client.send({"type": "autocomplete", "tab": tab_id, "request": 101, "query": "Stage2PerfBookmarkTarget"})
        ac1 = client.wait_for(lambda e: e.get("type") == "autocomplete_results" and e.get("request") == 101 and e.get("final") is True)
        assert any("stage2" in item.get("text", "") for item in ac1.get("items", [])), ac1

        client.send({"type": "toggle_bookmark", "tab": tab_id})
        client.wait_for(lambda e: e.get("type") == "bookmarks")

        client.send({"type": "autocomplete", "tab": tab_id, "request": 102, "query": "Stage2PerfBookmarkTarget"})
        ac2 = client.wait_for(lambda e: e.get("type") == "autocomplete_results" and e.get("request") == 102 and e.get("final") is True)
        assert not any("Stage2PerfBookmarkTarget" in item.get("title", "") for item in ac2.get("items", [])), ac2
        print("ok: autocomplete bookmark cache invalidation")

        # 4. Burst IPC commands in a single write (exercises offset-cursor framing).
        burst = b"".join(json.dumps({"type": "request_processes"}).encode() + b"\n" for _ in range(25))
        stream.sendall(burst)
        seen = 0
        end = time.time() + 15
        while seen < 25 and time.time() < end:
            ev = client.read_line(max(0.1, end - time.time()))
            if ev.get("type") == "processes":
                seen += 1
        assert seen == 25, f"expected 25 processes responses, got {seen}"
        print("ok: burst IPC offset-cursor framing")

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
