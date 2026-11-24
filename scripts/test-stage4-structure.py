#!/usr/bin/env python3
# Regression tests for Stage 4 structural & upstream-diff reduction:
# - Upstream LibRequests/CertificateInfo and vcpkg-running.lock are removed.
# - Direct OpenSSL CertificateInspector inspects a local HTTPS server certificate.
# - Resolver file change watch restarts RequestServer via ProcessManager directly.
import json
import os
import socket
import ssl
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

PAGE = b"""<!doctype html><html><body><h1>Stage 4 Structure</h1></body></html>"""


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

    # 1. Verify upstream CertificateInfo files are removed.
    assert not os.path.exists(os.path.join(ROOT, "Libraries/LibRequests/CertificateInfo.h"))
    assert not os.path.exists(os.path.join(ROOT, "Libraries/LibRequests/CertificateInfo.cpp"))
    print("ok: upstream LibRequests/CertificateInfo removed")

    tmp = tempfile.mkdtemp(prefix="ladybird-stage4-test-")
    resolver_file = os.path.join(tmp, "resolv.conf")
    with open(resolver_file, "w") as f:
        f.write("nameserver 127.0.0.1\n")

    # Generate a self-signed certificate for local HTTPS certificate inspection.
    cert_path = os.path.join(tmp, "cert.pem")
    key_path = os.path.join(tmp, "key.pem")
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", key_path, "-out", cert_path,
            "-days", "1", "-nodes", "-subj", "/CN=stage4-test.local",
        ],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    https_server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    https_port = https_server.server_address[1]
    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile=cert_path, keyfile=key_path)
    https_server.socket = ssl_ctx.wrap_socket(https_server.socket, server_side=True)
    https_thread = threading.Thread(target=https_server.serve_forever, daemon=True)
    https_thread.start()

    http_server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    http_port = http_server.server_address[1]
    http_thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    http_thread.start()

    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(os.path.join(tmp, "engine.sock"))
    listener.listen(1)

    env = dict(os.environ)
    env["SERENITY_RES_ROOT"] = SERENITY_RES
    env["LADYBIRD_SOURCE_DIR"] = ROOT
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")
    env["XDG_DATA_HOME"] = os.path.join(tmp, "data")
    env["LADYBIRD_SDE_RESOLVER_FILES"] = resolver_file
    for key in ("LD_LIBRARY_PATH", "WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(key, None)

    engine_log_path = os.path.join(tmp, "engine.log")
    engine_log = open(engine_log_path, "wb")
    engine = subprocess.Popen(
        [ENGINE, "--engine-socket", os.path.join(tmp, "engine.sock"),
         "--frame-directory", tmp, "--profile", "stage4-test", "--disable-sandbox"],
        env=env, stdout=engine_log, stderr=subprocess.STDOUT,
    )

    try:
        listener.settimeout(20)
        stream, _ = listener.accept()
        stream.settimeout(30)
        client = Client(stream)

        client.wait_for(lambda e: e.get("type") == "hello")

        # 2. Load a local HTTP page so RequestServer is spawned, then modify the watched resolver file
        #    and confirm the engine logs the restart and remains healthy.
        client.send({
            "type": "create_tab", "request": 1, "width": 800, "height": 600,
            "scale": 1, "url": f"http://127.0.0.1:{http_port}/", "private": False,
        })
        created = client.wait_for(lambda e: e.get("type") == "tab_created" and e.get("request") == 1)
        tab_id = created["tab"]
        client.wait_for(lambda e: e.get("type") == "frame" and e.get("tab") == tab_id)

        time.sleep(0.2)
        with open(resolver_file, "w") as f:
            f.write("nameserver 127.0.0.1\n# changed\n")

        # Wait up to 4 seconds for the 2-second resolver watch timer to fire.
        deadline = time.time() + 4.5
        restarted = False
        while time.time() < deadline:
            with open(engine_log_path, "r", errors="replace") as lf:
                if "resolver configuration changed; restarting RequestServers" in lf.read():
                    restarted = True
                    break
            time.sleep(0.2)
        assert restarted, "expected resolver change restart log"
        print("ok: resolver-change RequestServer restart via ProcessManager")

        # 3. Navigate to local self-signed HTTPS server and inspect its certificate via CertificateInspector.
        https_url = f"https://127.0.0.1:{https_port}/"
        client.send({"type": "navigate", "tab": tab_id, "url": https_url, "user_input": True})
        client.wait_for(lambda e: e.get("type") == "url" and e.get("tab") == tab_id and f":{https_port}" in e.get("url", ""))
        client.send({"type": "certificate_info", "tab": tab_id})
        cert = client.wait_for(lambda e: e.get("type") == "certificate_info" and e.get("tab") == tab_id, timeout=15)
        assert cert.get("ok") is True, cert
        assert "stage4-test.local" in cert.get("subject", ""), cert
        assert len(cert.get("sha256_fingerprint", "")) > 20, cert
        assert cert.get("protocol", "").startswith("TLS"), cert
        assert len(cert.get("cipher", "")) > 0, cert
        assert cert.get("verified") is False, cert
        assert len(cert.get("verification_error", "")) > 0, cert
        print("ok: standalone OpenSSL CertificateInspector (with verification status)")

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
        https_server.shutdown()
        http_server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
