#!/usr/bin/env python3
# Regression test for memfd_create + SCM_RIGHTS frame buffer migration:
# - Engine passes frame buffers as anonymous memfds via SCM_RIGHTS ("fd": true).
# - Zero /dev/shm/ladybird-sde-* named shared-memory files are created.
# - Received memfd has valid backing size and contains rendered frame pixels.
import array
import fcntl
import glob
import json
import mmap
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

PAGE = b"""<!doctype html><html>
<body style="margin:0;width:800px;height:600px;background:#1188cc">
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


class FdClient:
    def __init__(self, stream):
        self.stream = stream
        self.buffer = b""
        self.fds = []

    def read_line(self, deadline=30):
        end = time.time() + deadline
        while b"\n" not in self.buffer:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("timed out waiting for an engine event")
            self.stream.settimeout(max(0.1, left))
            chunk, ancdata, _flags, _addr = self.stream.recvmsg(65536, socket.CMSG_SPACE(16 * array.array("i").itemsize))
            if not chunk:
                raise EOFError("engine closed the control socket")
            for cmsg_level, cmsg_type, cmsg_data in ancdata:
                if cmsg_level == socket.SOL_SOCKET and cmsg_type == socket.SCM_RIGHTS:
                    received = array.array("i")
                    received.frombytes(cmsg_data[: len(cmsg_data) - (len(cmsg_data) % received.itemsize)])
                    self.fds.extend(received)
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

    def pop_fd(self):
        assert self.fds, "expected an SCM_RIGHTS file descriptor"
        return self.fds.pop(0)

    def send(self, event):
        self.stream.sendall(json.dumps(event).encode() + b"\n")

    def close_fds(self):
        for fd in self.fds:
            os.close(fd)
        self.fds.clear()


def main():
    if not os.path.exists(ENGINE):
        print(f"error: engine host not found at {ENGINE}", file=sys.stderr)
        return 1

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    tmp = tempfile.mkdtemp(prefix="ladybird-memfd-test-")
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
         "--frame-directory", tmp, "--profile", "memfd-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    client = None
    fb_fd = -1
    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        client = FdClient(stream)

        client.wait_for(lambda e: e.get("type") == "hello")

        client.send({
            "type": "create_tab", "request": 1, "width": 800, "height": 600,
            "scale": 1, "url": f"http://127.0.0.1:{port}/", "private": False,
        })
        created = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 1)
        tab_id = created["tab"]

        fb = client.wait_for(lambda e: e.get("type") == "frame_buffer" and e.get("tab") == tab_id)
        assert fb.get("fd") is True, f"expected fd=true memfd frame_buffer, got {fb}"
        assert "name" not in fb, f"expected no /dev/shm name in frame_buffer, got {fb}"

        fb_fd = client.pop_fd()
        expected_size = fb["slot_size"] * fb["slots"]
        st = os.fstat(fb_fd)
        assert st.st_size >= expected_size, f"memfd size {st.st_size} < expected {expected_size}"
        seals = fcntl.fcntl(fb_fd, fcntl.F_GET_SEALS)
        expected_seals = fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL
        assert (seals & expected_seals) == expected_seals, f"expected seals {expected_seals:#x}, got {seals:#x}"
        print(f"ok: received sealed SCM_RIGHTS memfd ({st.st_size} bytes, {fb['slots']} slots, seals={seals:#x})")

        # Verify no /dev/shm/ladybird-sde-<engine_pid>-* file was created.
        shm_matches = glob.glob(f"/dev/shm/ladybird-sde-{engine.pid}-*")
        assert not shm_matches, f"unexpected /dev/shm files: {shm_matches}"
        print("ok: zero /dev/shm named objects created")

        frame = client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab_id)
        slot_offset = frame["slot"] * fb["slot_size"]
        with mmap.mmap(fb_fd, expected_size, mmap.MAP_SHARED, mmap.PROT_READ) as mm:
            # Sample a pixel from the painted slot (BGRA8888); should not be all zeros.
            sample = mm[slot_offset : slot_offset + 4096]
            assert any(b != 0 for b in sample), "expected non-zero painted pixels in memfd"
        client.send({"type": "frame_ack", "tab": tab_id, "sequence": frame["sequence"]})
        print("ok: read-only mmap of memfd contains rendered frame pixels")

        client.send({"type": "quit"})
        engine.wait(timeout=10)
        print("PASS")
        return 0
    finally:
        if fb_fd >= 0:
            os.close(fb_fd)
        if client is not None:
            client.close_fds()
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
