#!/usr/bin/env python3
# A tiny stand-in for the Ladybird engine host, used to exercise the LibGUI
# frontend without a Ladybird build. It speaks the same control protocol and
# publishes a synthetic frame through the same POSIX shared-memory buffer.
#
# This is a test aid, not a browser. The real engine host is LadybirdEngine.
import json
import os
import socket
import struct
import sys

def parse_args(argv):
    result = {}
    index = 1
    while index < len(argv):
        if argv[index] == "--engine-socket":
            result["socket"] = argv[index + 1]
            index += 2
        elif argv[index] == "--frame-directory":
            result["frames"] = argv[index + 1]
            index += 2
        else:
            index += 1
    return result

def make_frame(width, height, index):
    # A calm blue-grid placeholder, so a screenshot is recognisably the
    # LibGUI frontend rather than an empty window. Build rows by repetition;
    # a per-pixel Python loop is too slow once a real window resizes.
    background = bytes((252, 248, 246, 255))   # BGRA
    grid = bytes((242, 232, 225, 255))
    border = bytes((120, 70, 40, 255))
    rows = []
    for y in range(height):
        if y < 3 or y >= height - 3:
            rows.append(border * width)
        elif y % 64 == 0:
            rows.append(grid * width)
        else:
            rows.append(border * 3 + background * (width - 6) + border * 3)
    return b"".join(rows)

def main():
    args = parse_args(sys.argv)
    socket_path = args.get("socket")
    if not socket_path:
        print("fake-engine: --engine-socket is required", file=sys.stderr)
        return 1

    print(f"fake-engine: connecting to {socket_path}", file=sys.stderr, flush=True)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_path)
    print("fake-engine: connected", file=sys.stderr, flush=True)

    shm_name = f"/ladybird-sde-fake-{os.getpid()}"
    shm_path = f"/dev/shm/{shm_name[1:]}"
    width, height = 1024, 768
    slots = 3
    slot_size = 1920 * 1080 * 4
    frame_data = make_frame(width, height, 0)

    try:
        os.unlink(shm_path)
    except FileNotFoundError:
        pass
    fd = os.open(shm_path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    os.ftruncate(fd, slot_size * slots)
    os.close(fd)

    def send(message):
        sock.sendall((json.dumps(message) + "\n").encode())

    def publish(tab_id):
        data = frame_data
        fd = os.open(shm_path, os.O_RDWR)
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, data)
        os.close(fd)
        send({"type": "frame", "tab": tab_id, "slot": 0, "width": width, "height": height,
              "stride": width * 4, "sequence": 1, "full": True,
              "damage_x": 0, "damage_y": 0, "damage_width": width, "damage_height": height})

    send({"type": "hello", "protocol": 5})
    send({"type": "downloads", "items": []})
    send({"type": "bookmarks", "items": [
        {"title": "SerenityOS", "url": "https://serenityos.org/"},
        {"title": "Ladybird", "url": "https://ladybird.org/"},
    ]})
    print("fake-engine: hello sent", file=sys.stderr, flush=True)

    buffer = b""
    running = True
    while running:
        print("fake-engine: waiting for command", file=sys.stderr, flush=True)
        chunk = sock.recv(65536)
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            print(f"fake-engine: command {line!r}", file=sys.stderr, flush=True)
            try:
                event = json.loads(line)
            except Exception:
                continue
            kind = event.get("type")
            if kind == "create_tab":
                tab_id = 1
                send({"type": "tab_created", "tab": tab_id, "request": event.get("request", 0),
                      "width": width, "height": height})
                send({"type": "frame_buffer", "tab": tab_id, "name": shm_name, "slots": slots,
                      "slot_size": slot_size, "width": width, "height": height})
                send({"type": "title", "tab": tab_id, "title": "Ladybird — LibGUI frontend"})
                send({"type": "url", "tab": tab_id, "url": "https://example.com/", "bookmarked": False})
                send({"type": "loading", "tab": tab_id, "loading": False})
                send({"type": "history", "tab": tab_id, "can_go_back": False, "can_go_forward": False})
                publish(tab_id)
            elif kind == "resize":
                new_width = int(event.get("width", width))
                new_height = int(event.get("height", height))
                if (new_width, new_height) != (width, height):
                    width, height = new_width, new_height
                    frame_data = make_frame(width, height, 0)
                    publish(event.get("tab", 1))
            elif kind == "navigate":
                send({"type": "url", "tab": event.get("tab", 1), "url": event.get("url", ""), "bookmarked": False})
            elif kind == "quit":
                running = False
                break

    try:
        os.unlink(shm_path)
    except FileNotFoundError:
        pass
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
