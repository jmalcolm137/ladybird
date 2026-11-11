#!/usr/bin/env python3
# Frontend protocol fuzzer for the LibGUI Ladybird port.
#
# This targets *our* integration code, not upstream Ladybird: it launches the
# real LibGUI frontend under a headless SDE compositor, but replaces the engine
# host with a fake engine that speaks the JSON protocol in Shared/EngineProtocol.h.
#
# The fake engine first answers the frontend's commands well enough to create a
# tab, then emits a seed-driven stream of malformed, mistyped, out-of-range and
# out-of-order events. A crash, fatal signal, or a thread stuck in a socket send
# fails the run; the seed and every event are kept for replay/minimisation.
#
# Usage:
#   scripts/fuzz-frontend.py                      # 10 seeds, 8 s each
#   scripts/fuzz-frontend.py --seeds 1-50 --duration 15
#   scripts/fuzz-frontend.py --seed 123 --keep    # replay one seed, keep logs
#   scripts/fuzz-frontend.py --modals             # include dialog-opening events
#   FUZZ_TITLE_LENGTH=65536 scripts/fuzz-frontend.py --only title --only-once
#                                                 # long-title regression probe
#
# The same file is executed by the frontend as the fake engine; it spots the
# --engine-socket argument and switches to engine mode.

import argparse
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CHROME = os.path.join(ROOT, "build/live/bin/Ladybird")
SDE_ROOT = os.path.abspath(os.path.join(ROOT, "..", "serenity-desktop-environment"))
DEFAULT_COMPOSITOR = os.path.join(SDE_ROOT, "build/WindowServer/WindowServer")
LAGOM = os.path.abspath(os.path.join(ROOT, "..", "libgui-wayland/build/lagom"))
SERENITY_RES = os.path.abspath(os.path.join(ROOT, "..", "libgui-wayland/serenity/Base/res"))

PROTOCOL_VERSION = 18


# ---------------------------------------------------------------------------
# Fake engine (runs as a child of the frontend)
# ---------------------------------------------------------------------------

class FakeEngine:
    def __init__(self, socket_path, seed, duration, mode, log_path, command_log, modals):
        self.socket_path = socket_path
        self.random = random.Random(seed)
        self.duration = duration
        self.mode = mode
        self.log_path = log_path
        self.command_log = command_log
        self.modals = modals
        self.scenario = os.environ.get("FUZZ_SCENARIO", "protocol")
        self.sock = None
        self.write_lock = threading.Lock()
        self.stop = threading.Event()
        self.tabs = []
        self.next_tab = 1
        self.ready_path = os.environ.get("FUZZ_READY_FILE", "")
        self.only = os.environ.get("FUZZ_ONLY", "")
        self.only_once = os.environ.get("FUZZ_ONLY_ONCE", "0") == "1"

    # -- transport -----------------------------------------------------------
    def connect(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.socket_path)
                self.sock = s
                s.settimeout(0.2)
                if self.ready_path:
                    with open(self.ready_path, "w") as f:
                        f.write("connected\n")
                return True
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                time.sleep(0.05)
        return False

    def send_bytes(self, payload):
        if not self.sock:
            return
        with self.write_lock:
            try:
                self.sock.sendall(payload)
            except OSError:
                self.stop.set()

    def send(self, event):
        line = json.dumps(event, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"
        self.log_event(line)
        self.send_bytes(line)

    def log_event(self, line):
        if not self.log_path:
            return
        try:
            with open(self.log_path, "ab") as f:
                if len(line) > 4096:
                    f.write(line[:4096] + b"...[%d bytes]\n" % len(line))
                else:
                    f.write(line)
        except OSError:
            pass

    def log_command(self, line):
        if not self.command_log:
            return
        try:
            with open(self.command_log, "ab") as f:
                f.write(line + b"\n")
        except OSError:
            pass

    # -- command handling ----------------------------------------------------
    def reply(self, tab):
        self.send({"type": "url", "tab": tab, "url": "about:blank", "bookmarked": False})
        self.send({"type": "title", "tab": tab, "title": "fuzz"})
        self.send({"type": "loading", "tab": tab, "loading": False})
        self.send({"type": "history", "tab": tab, "can_go_back": False, "can_go_forward": False})

    def handle_command(self, line):
        self.log_command(line)
        try:
            command = json.loads(line)
        except Exception:
            return
        if not isinstance(command, dict):
            return
        kind = command.get("type")
        tab = command.get("tab")

        if kind == "create_tab":
            tab = self.next_tab
            self.next_tab += 1
            self.tabs.append(tab)
            self.send({"type": "tab_created", "request": command.get("request", 0), "tab": tab})
            self.reply(tab)
            # Some UI code asks for a favicon/title immediately after creation.
            self.send({"type": "favicon", "tab": tab, "path": ""})
        elif kind == "navigate":
            if tab in self.tabs:
                self.send({"type": "url", "tab": tab, "url": command.get("url", ""), "bookmarked": False})
                self.send({"type": "loading", "tab": tab, "loading": True})
            self.send({"type": "title", "tab": tab, "title": "fuzz"})
            self.send({"type": "loading", "tab": tab, "loading": False})
        elif kind == "autocomplete":
            self.send({"type": "autocomplete_results", "tab": tab,
                       "request": command.get("request", 0), "items": [], "final": True})
        elif kind in ("history_recent",):
            self.send({"type": "history_recent", "entries": []})
        elif kind in ("history_search",):
            self.send({"type": "history_search_results", "query": command.get("query", ""),
                       "entries": [], "items": []})
        elif kind in ("bookmarks", "toggle_bookmark", "add_bookmark", "edit_bookmark",
                      "remove_bookmark", "add_folder", "edit_folder", "move_bookmark"):
            self.send({"type": "bookmarks", "items": []})
        elif kind == "request_processes":
            self.send({"type": "processes", "children": []})
        elif kind in ("downloads",):
            self.send({"type": "downloads", "items": []})
        elif kind == "find":
            self.send({"type": "find_result", "tab": tab, "current": 0, "total": 0})
        elif kind == "quit":
            self.stop.set()

    def reader(self):
        buffer = b""
        while not self.stop.is_set() and self.sock:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if line:
                    self.handle_command(line)
        self.stop.set()

    # -- event corpus --------------------------------------------------------
    def base_events(self, tab):
        events = [
            {"type": "title", "tab": tab, "title": "x" * int(os.environ.get("FUZZ_TITLE_LENGTH", "4"))},
            {"type": "url", "tab": tab, "url": "https://example.com/", "bookmarked": False},
            {"type": "loading", "tab": tab, "loading": True},
            {"type": "history", "tab": tab, "can_go_back": True, "can_go_forward": False},
            {"type": "audio", "tab": tab, "playing": False, "muted": False},
            {"type": "zoom_changed", "tab": tab, "zoom": 1.0},
            {"type": "autocomplete_results", "tab": tab, "request": 1, "items": [], "final": True},
            {"type": "find_result", "tab": tab, "current": 1, "total": 1},
            {"type": "favicon", "tab": tab, "path": ""},
            {"type": "cursor", "tab": tab, "cursor": "default"},
            {"type": "link_hover", "tab": tab, "url": "https://example.com/"},
            {"type": "input_method_state", "tab": tab, "enabled": False},
            {"type": "tab_unresponsive", "tab": tab, "unresponsive": True},
            {"type": "tab_unresponsive", "tab": tab, "unresponsive": False},
            {"type": "clipboard", "tab": tab, "text": "fuzz"},
            # certificate_info, tab_crashed, compat_report_* and the drag/menu
            # events open dialogs or nested event loops; the input-driven
            # scenarios exercise those with a way to dismiss them.
            # tab_closed/open_window are deliberately not in the default corpus:
            # they end the tab/window under test and turn the run into a
            # lifecycle test. They get their own scenario below.
            {"type": "session_tab_registered", "request": 1, "session_window": 1, "window": 1, "url": "about:blank"},
            {"type": "user_agent_changed", "value": "fuzz-agent"},
            {"type": "navigator_compatibility_mode_changed", "value": "chrome"},
            {"type": "view_preferences", "website_color_scheme": "auto", "preferred_contrast": "auto", "preferred_motion": "auto"},
            {"type": "devtools_error", "message": "fuzz"},
            {"type": "process_error", "message": "fuzz"},
            {"type": "downloads", "items": []},
            {"type": "bookmarks", "items": []},
            {"type": "history_recent", "entries": []},
            {"type": "session_window", "window": 1, "x": 0, "y": 0, "width": 800, "height": 600, "maximized": False},
        ]
        if self.only:
            events = [event for event in events if event.get("type") == self.only]
        if self.modals:
            events += [
                {"type": "permission", "tab": tab, "request": 1, "name": "geolocation", "origin": "https://example.com"},
                {"type": "dialog", "tab": tab, "kind": "alert", "message": "fuzz", "default": ""},
                {"type": "file_picker", "tab": tab, "multiple": False, "filters": []},
                {"type": "certificate_info", "tab": tab, "valid": True, "subject": "fuzz", "issuer": "fuzz",
                 "not_before": "2020-01-01", "not_after": "2030-01-01", "protocol": "TLS", "cipher": "fuzz"},
                {"type": "compat_report_ready", "tab": tab,
                 "url": "https://example.com/", "title": "fuzz", "description": "fuzz"},
            ]
        return events

    def mutate(self, value, depth=0):
        if depth > 3:
            return value
        if isinstance(value, dict):
            out = {}
            for key, item in value.items():
                action = self.random.random()
                if action < 0.10:
                    continue
                elif action < 0.20:
                    out[key] = self.random_scalar()
                else:
                    out[key] = self.mutate(item, depth + 1)
            if self.random.random() < 0.10:
                out[self.random.choice(["extra", "tab", "type", "url", "x", "y"])] = self.random_scalar()
            return out
        if isinstance(value, list):
            if self.random.random() < 0.15:
                return self.random_scalar()
            return [self.mutate(item, depth + 1) for item in value]
        if self.random.random() < 0.15:
            return self.random_scalar()
        return value

    def random_scalar(self):
        choice = self.random.randrange(12)
        if choice == 0:
            return None
        if choice == 1:
            return True
        if choice == 2:
            return False
        if choice == 3:
            return 0
        if choice == 4:
            return -1
        if choice == 5:
            return 2**63 - 1
        if choice == 6:
            return -(2**63)
        if choice == 7:
            return 1e308
        if choice == 8:
            return ""
        if choice == 9:
            return "x" * self.random.randrange(0, 512)
        if choice == 10:
            return []
        return {}

    def raw_line(self):
        choice = self.random.randrange(10)
        if choice == 0:
            return b"{"
        if choice == 1:
            return b"[]"
        if choice == 2:
            return b"null"
        if choice == 3:
            return b'{"type":'
        if choice == 4:
            return b'{"type":"title","tab":1,"title":"\xff\xfe"}'
        if choice == 5:
            return b'{"type":"title","tab":1,"title":"' + b"A" * self.random.randrange(1, 8192) + b'"}'
        if choice == 6:
            return b"{" + b'"type":"title",' + b'"unknown":[' * 32 + b'0' + b']}' * 32 + b"}"
        if choice == 7:
            return b'{"type":"hello","protocol":17}'
        if choice == 8:
            return b'{"type":"hello","protocol":18}'
        return b'{"type":1,"tab":-1}'

    def fuzz_once(self):
        if self.mode == "quiet":
            time.sleep(0.1)
            return
        tab = self.tabs[0] if self.tabs else 1
        if self.only:
            events = self.base_events(tab)
            if events:
                self.send(events[0])
            if self.only_once:
                self.only = ""
                self.mode = "quiet"
            time.sleep(0.2)
            return
        if self.mode == "raw" or self.random.random() < 0.25:
            payload = self.raw_line()
            if self.random.random() < 0.15:
                # Split a line so the reader has to buffer a partial message.
                cut = self.random.randrange(1, max(2, len(payload)))
                self.send_bytes(payload[:cut])
                time.sleep(0.01)
                self.send_bytes(payload[cut:] + b"\n")
                return
            self.log_event(payload + b"\n")
            self.send_bytes(payload + b"\n")
            return

        base = self.random.choice(self.base_events(tab))
        event = self.mutate(base)
        if not isinstance(event, dict):
            event = {"type": "title", "tab": tab, "title": "fuzz"}
        self.send(event)

    def wait_for_tab(self, timeout=6):
        end = time.time() + timeout
        while time.time() < end and not self.tabs and not self.stop.is_set():
            time.sleep(0.05)
        return bool(self.tabs)

    def run_lifecycle(self):
        if not self.wait_for_tab():
            return
        tab1 = self.tabs[0]
        self.send({"type": "open_window", "url": "about:blank"})
        end = time.time() + 3
        while len(self.tabs) < 2 and time.time() < end and not self.stop.is_set():
            time.sleep(0.05)
        tab2 = self.tabs[-1]

        sequence = [
            {"type": "session_window", "window": 1, "session_window": 1},
            {"type": "session_window", "window": 2, "session_window": 2},
            {"type": "title", "tab": tab1, "title": "lifecycle one"},
            {"type": "url", "tab": tab2, "url": "about:blank", "bookmarked": False},
            {"type": "fullscreen", "tab": tab1, "fullscreen": True},
            {"type": "fullscreen", "tab": tab1, "fullscreen": False},
            {"type": "history_search_results", "query": "lifecycle", "items": []},
            {"type": "certificate_info", "tab": tab1, "valid": False},
            {"type": "file_picker", "tab": tab1, "multiple": False, "filters": []},
            {"type": "permission", "tab": tab1, "request": 1, "name": "geolocation", "origin": "https://example.com"},
            {"type": "context_menu", "tab": tab1, "x": 10, "y": 10, "items": []},
            {"type": "select_dropdown", "tab": tab1, "x": 10, "y": 10, "min_width": 100, "items": []},
            {"type": "dialog", "tab": tab2, "kind": "alert", "message": "lifecycle", "default": ""},
            {"type": "clipboard", "tab": tab1, "text": "lifecycle"},
            {"type": "tab_closed", "tab": tab1},
            {"type": "open_window", "url": "about:blank"},
        ]
        for event in sequence:
            if self.stop.is_set():
                break
            self.send(event)
            time.sleep(0.5)
        while not self.stop.is_set():
            time.sleep(0.1)

    def run(self):
        if not self.connect():
            return 1
        # Handshake first; without it the frontend will not accept tab events.
        self.send({"type": "hello", "protocol": PROTOCOL_VERSION})
        self.send({"type": "user_agents", "current": "fuzz", "agents": []})
        self.send({"type": "navigator_compatibility_modes", "current": "chrome", "modes": []})

        reader = threading.Thread(target=self.reader, daemon=True)
        reader.start()

        if self.scenario == "lifecycle":
            self.run_lifecycle()
        else:
            deadline = time.time() + self.duration
            while time.time() < deadline and not self.stop.is_set():
                self.fuzz_once()
                time.sleep(self.random.choice([0.0002, 0.001, 0.005, 0.02]))

        # Stay attached so the frontend sees a live peer while it shuts down.
        hard_deadline = time.time() + 30
        while time.time() < hard_deadline and not self.stop.is_set():
            time.sleep(0.05)
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        return 0


def fake_engine_main(argv):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--engine-socket", required=True)
    parser.add_argument("--frame-directory", default="")
    parser.add_argument("--profile", default="")
    parser.add_argument("--devtools", nargs="?", default=None)
    parser.add_argument("--disable-sandbox", action="store_true")
    args, _ = parser.parse_known_args(argv)

    seed = int(os.environ.get("FUZZ_SEED", "1"))
    duration = float(os.environ.get("FUZZ_DURATION", "8"))
    mode = os.environ.get("FUZZ_MODE", "mutate")
    log_path = os.environ.get("FUZZ_ACTION_LOG", "")
    command_log = os.environ.get("FUZZ_COMMAND_LOG", "")
    modals = os.environ.get("FUZZ_MODALS", "0") == "1"

    engine = FakeEngine(args.engine_socket, seed, duration, mode, log_path, command_log, modals)
    try:
        return engine.run()
    except Exception:
        traceback.print_exc()
        return 2


# ---------------------------------------------------------------------------
# Orchestrator
# ---------------------------------------------------------------------------

def wait_for_path(path, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def parse_seeds(text):
    seeds = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            seeds.extend(range(int(a), int(b) + 1))
        else:
            seeds.append(int(part))
    return seeds


def sample_threads(pid):
    # Return a list of (tid, comm, state, wchan) for a process.
    out = []
    try:
        for tid in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                comm = f.read().strip()
            with open(f"/proc/{pid}/task/{tid}/wchan") as f:
                wchan = f.read().strip()
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                state = f.read().split()[2]
            out.append((tid, comm, state, wchan))
    except OSError:
        pass
    return out


def escape_keypresses(socket_name, runtime, stop, duration):
    # One virtual-input connection for the whole run. Creating a virtual
    # keyboard per press destroys the seat's keyboard focus between presses,
    # so after the first Escape the rest would go nowhere.
    vi = os.path.join(SDE_ROOT, "build/input-test/virtual-input-test")
    if not os.access(vi, os.X_OK):
        return
    env = dict(os.environ)
    env.update({
        "WAYLAND_DISPLAY": socket_name,
        "XDG_RUNTIME_DIR": os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"),
        "SDE_STATE_DIR": os.path.join(runtime, "state"),
        "SDE_WM_PORTAL_PATH": os.path.join(runtime, "wm"),
        "SERENITY_RES_ROOT": SERENITY_RES,
        "LD_LIBRARY_PATH": os.path.join(LAGOM, "lib"),
    })
    env.pop("DISPLAY", None)
    args = [vi, "--extent", "1280", "720"]
    for _ in range(max(1, int(duration / 0.5) + 2)):
        args += ["--key", "1", "press", "--sleep", "60", "--key", "1", "release", "--sleep", "350"]
    args += ["--linger", "500"]
    try:
        process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        return
    while not stop.wait(0.2):
        if process.poll() is not None:
            break
    try:
        process.terminate()
        process.wait(timeout=2)
    except Exception:
        try:
            process.kill()
        except Exception:
            pass


def last_exit_reason(log_path):
    try:
        with open(log_path, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return ""
    for line in reversed(lines):
        if "Ladybird: exiting:" in line:
            return line.strip()
    return ""


def run_one(seed, duration, args):
    runtime = tempfile.mkdtemp(prefix=f"ladybird-fuzz-{seed}-")
    try:
        os.makedirs(os.path.join(runtime, "data"))
        os.makedirs(os.path.join(runtime, "config"))
        os.makedirs(os.path.join(runtime, "state"))
        socket_name = f"fuzz-{os.getpid()}-{seed}"
        xdg_runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
        socket_path = os.path.join(xdg_runtime, socket_name)

        comp_env = dict(os.environ)
        comp_env.update({
            "WLR_BACKENDS": "headless",
            "WLR_HEADLESS_OUTPUTS": "1",
            "XDG_RUNTIME_DIR": xdg_runtime,
            "XDG_DATA_HOME": os.path.join(runtime, "data"),
            "XDG_CONFIG_HOME": os.path.join(runtime, "config"),
            "SDE_STATE_DIR": os.path.join(runtime, "state"),
            "SDE_WM_PORTAL_PATH": os.path.join(runtime, "wm"),
            "SERENITY_RES_ROOT": SERENITY_RES,
            "LD_LIBRARY_PATH": os.path.join(LAGOM, "lib"),
        })
        for key in ("WAYLAND_DISPLAY", "DISPLAY"):
            comp_env.pop(key, None)
        comp_log = open(os.path.join(runtime, "compositor.log"), "wb")
        compositor = subprocess.Popen(
            [args.compositor, "-n", socket_name],
            env=comp_env, stdout=comp_log, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        if not wait_for_path(socket_path, 15):
            return {"seed": seed, "status": "compositor-failed", "runtime": runtime,
                    "detail": "compositor did not create its socket"}

        engine_script = os.path.abspath(args.engine)
        chrome_env = dict(comp_env)
        chrome_env.update({
            "WAYLAND_DISPLAY": socket_name,
            "SDE_APP_BIN_DIR": os.path.join(LAGOM, "bin"),
            "LADYBIRD_SOURCE_DIR": ROOT,
            "LADYBIRD_ENGINE_HOST": engine_script,
            "LADYBIRD_SDE_PROFILE": f"fuzz-{os.getpid()}-{seed}",
            "LADYBIRD_SDE_DISABLE_SESSION_RESTORE": "1",
            "FUZZ_SEED": str(seed),
            "FUZZ_DURATION": str(duration),
            "FUZZ_MODE": args.mode,
            "FUZZ_ACTION_LOG": os.path.join(runtime, "events.jsonl"),
            "FUZZ_COMMAND_LOG": os.path.join(runtime, "commands.jsonl"),
            "FUZZ_READY_FILE": os.path.join(runtime, "engine-connected"),
            "FUZZ_MODALS": "1" if (args.modals or args.scenario in ("modals", "lifecycle")) else "0",
            "FUZZ_SCENARIO": args.scenario,
            "FUZZ_ONLY": args.only,
            "FUZZ_ONLY_ONCE": "1" if args.only_once else "0",
        })
        chrome_env.pop("LD_LIBRARY_PATH", None)
        chrome_log_path = os.path.join(runtime, "chrome.log")
        chrome_log = open(chrome_log_path, "wb")
        chrome = subprocess.Popen(
            [args.chrome, "about:blank"],
            env=chrome_env, stdout=chrome_log, stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        escape_stop = threading.Event()
        escape_thread = None
        if args.scenario in ("lifecycle", "modals") or args.modals:
            escape_thread = threading.Thread(
                target=escape_keypresses, args=(socket_name, runtime, escape_stop, duration), daemon=True)
            escape_thread.start()

        if not wait_for_path(os.path.join(runtime, "engine-connected"), 15):
            status = "engine-not-connected"
        else:
            # Let the window map and the first tab get created.
            time.sleep(1.0)
            status = "ok"
            deadline = time.time() + duration + 4
            stalled_since = None
            while time.time() < deadline:
                if chrome.poll() is not None:
                    reason = last_exit_reason(chrome_log_path)
                    if "last window closed" in reason or "quit action" in reason:
                        status = "expected-exit" if args.scenario in ("lifecycle", "modals") else "unexpected-exit"
                    else:
                        status = "crashed"
                    break
                stalled = False
                for _, _, state, wchan in sample_threads(chrome.pid):
                    if "sock_alloc_send_pskb" in wchan or "sendto" in wchan or "tcp_sendmsg" in wchan:
                        stalled = True
                if stalled:
                    stalled_since = stalled_since or time.time()
                    if time.time() - stalled_since > 3:
                        status = "hung-send"
                        break
                else:
                    stalled_since = None
                time.sleep(0.1)
            if status == "ok" and chrome.poll() is not None:
                reason = last_exit_reason(chrome_log_path)
                if "last window closed" in reason or "quit action" in reason:
                    status = "expected-exit" if args.scenario in ("lifecycle", "modals") else "unexpected-exit"
                else:
                    status = "crashed"

        # Let a final Escape close any dialog left open by the scenario.
        escape_stop.set()
        if escape_thread:
            escape_thread.join(timeout=2)

        # Try a graceful stop, then a core dump if it will not leave.
        result = {"seed": seed, "status": status, "runtime": runtime}
        if chrome.poll() is None:
            try:
                chrome.send_signal(signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                code = chrome.wait(timeout=5)
                result["exit"] = code
            except subprocess.TimeoutExpired:
                result["status"] = "hung-exit"
                try:
                    chrome.send_signal(signal.SIGXCPU)
                except ProcessLookupError:
                    pass
                try:
                    chrome.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        else:
            result["exit"] = chrome.returncode

        if result["status"] != "ok":
            result["log"] = chrome_log_path
            result["core_hint"] = f"coredumpctl dump {chrome.pid} -o {runtime}/core"
        return result

    finally:
        try:
            escape_stop.set()
        except Exception:
            pass
        try:
            compositor.terminate()
        except Exception:
            pass
        try:
            compositor.wait(timeout=3)
        except Exception:
            pass
        try:
            os.unlink(socket_path)
        except OSError:
            pass
        comp_log.close()
        try:
            chrome_log.close()
        except Exception:
            pass


def orchestrator_main(argv):
    parser = argparse.ArgumentParser(description="Fuzz the LibGUI frontend's engine protocol integration")
    parser.add_argument("--seeds", default="1-10", help="comma/range list, e.g. 1-20 or 1,4,9")
    parser.add_argument("--seed", type=int, help="run one seed")
    parser.add_argument("--duration", type=float, default=8.0, help="seconds of fuzzing per seed")
    parser.add_argument("--mode", choices=["mutate", "raw", "quiet"], default="mutate")
    parser.add_argument("--scenario", choices=["protocol", "lifecycle", "modals"], default="protocol",
                        help="protocol = event fuzz; lifecycle = window/tab/dialog sequence; modals = protocol with dialogs")
    parser.add_argument("--modals", action="store_true", help="include dialog-opening events")
    parser.add_argument("--only", default="", help="send only this event type (diagnostic)")
    parser.add_argument("--only-once", action="store_true", help="send the --only event once")
    parser.add_argument("--keep", action="store_true", help="keep the runtime directory")
    parser.add_argument("--chrome", default=DEFAULT_CHROME)
    parser.add_argument("--compositor", default=DEFAULT_COMPOSITOR)
    parser.add_argument("--engine", default=os.path.abspath(__file__),
                        help="path used as LADYBIRD_ENGINE_HOST")
    args = parser.parse_args(argv)

    if args.seed is not None:
        seeds = [args.seed]
    else:
        seeds = parse_seeds(args.seeds)

    if not os.access(args.chrome, os.X_OK):
        print(f"error: no executable chrome at {args.chrome}", file=sys.stderr)
        return 2
    if not os.access(args.compositor, os.X_OK):
        print(f"error: no executable compositor at {args.compositor}", file=sys.stderr)
        return 2

    failures = 0
    for seed in seeds:
        started = time.time()
        result = run_one(seed, args.duration, args)
        elapsed = time.time() - started
        if result["status"] in ("ok", "expected-exit"):
            note = " (window closed)" if result["status"] == "expected-exit" else ""
            print(f"ok   seed={seed} {elapsed:.1f}s{note}")
            if not args.keep:
                shutil.rmtree(result["runtime"], ignore_errors=True)
        else:
            failures += 1
            print(f"FAIL seed={seed} status={result['status']} {elapsed:.1f}s")
            print(f"     runtime: {result['runtime']}")
            if "log" in result:
                print(f"     log:     {result['log']}")
            if "exit" in result:
                print(f"     exit:    {result['exit']}")
            if "core_hint" in result:
                print(f"     core:    {result['core_hint']}")
            # Leave the runtime for triage even without --keep.
    if failures:
        print(f"{failures}/{len(seeds)} seeds failed", file=sys.stderr)
        return 1
    print(f"{len(seeds)} seeds passed")
    return 0


if __name__ == "__main__":
    if "--engine-socket" in sys.argv:
        sys.exit(fake_engine_main(sys.argv[1:]))
    sys.exit(orchestrator_main(sys.argv[1:]))
