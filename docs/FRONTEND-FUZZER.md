# Frontend fuzzer and stress harness

`scripts/fuzz-frontend.py` targets **our** integration code, not upstream
Ladybird:

- `UI/LibGUI/*` — windows, tabs, dialogs, menus, session, drag, clipboard.
- `UI/LadybirdEngine/*` and `Shared/EngineProtocol.h` — the engine-host glue.
- `libgui-wayland/src/*` — LibWM, the window portal, input and presentation.

The engine host is replaced by a fake engine. The real frontend, LibGUI,
LibWM, and the SDE compositor run normally.

## How it works

1. Start the SDE compositor with `WLR_BACKENDS=headless` on a private socket.
2. Start `build/live/bin/Ladybird` with an isolated `XDG_*` and profile, and
   `LADYBIRD_ENGINE_HOST` pointing back at this script.
3. The script detects `--engine-socket` and acts as the fake engine: it
   completes the handshake, answers `create_tab` and the other commands, then
   runs the selected scenario.
4. The driver watches the real frontend for fatal signals, cores, unexpected
   exits, and threads stuck in a socket send. On failure it keeps the seed,
   every event, every command, the frontend log, and a core-dump hint.

## Scenarios

| Scenario | What it drives |
| --- | --- |
| `protocol` (default) | Seed-driven mutations of every known event: wrong types, missing fields, huge/negative numbers, unknown IDs, partial lines, invalid UTF-8, duplicate `hello`, floods, giant strings. |
| `lifecycle` | Extra window, fullscreen toggle storm (backing-store rebuilds), history, certificate, file picker, permission, menu, dropdown, alert, clipboard, tab close, another window. |
| `modals` | The protocol corpus plus dialog-opening events; Escape is delivered continuously to dismiss them. |
| `close` | Clicks the compositor's frame close button and requires `last window closed`. |
| `resize` | Double-clicks the titlebar repeatedly to maximize and restore. |
| `resize-edge` | Drags the right frame edge repeatedly and requires the presented size to change. |
| `disconnect` | The fake engine drops its socket mid-session; the frontend must stay alive. |
| `windows` | Creates nine windows and requires them in the exit log. |
| `session` | Sends a three-window session plan, restores it, and requires the saved session to contain at least three windows. |

Escape and pointer input use **one long-lived virtual-input connection**.
Connecting a fresh virtual keyboard per press destroys the seat's keyboard
focus, so only the first Escape would arrive.

## Usage

```bash
# short protocol fuzz
scripts/fuzz-frontend.py --seeds 1-10 --duration 8

# one scenario
scripts/fuzz-frontend.py --scenario lifecycle --duration 15
scripts/fuzz-frontend.py --scenario modals --duration 6
scripts/fuzz-frontend.py --scenario close
scripts/fuzz-frontend.py --scenario resize-edge

# replay a failure
scripts/fuzz-frontend.py --seed 123 --keep

# CI-sized slice
scripts/test-frontend-fuzz.sh
```

Useful environment variables:

- `LADYBIRD_SDE_DEBUG_QUIT=1` — print a backtrace at `Application::quit` and
  the last-window quit path (LibGUI patch 0089).
- `FUZZ_TITLE_LENGTH=N` with `--only title` — the long-title regression probe.
- `ASAN_OPTIONS=detect_leaks=0` with an ASAN build.
- `--chrome` / `--compositor` — point at another build.

## Failures found so far

- A 4096-byte window title creates a Wayland message larger than libwayland's
  4 KiB connection buffer; the compositor resets the connection and the
  frontend exits as if it crashed. LibWM now clamps titles to 1024 bytes at a
  UTF-8 boundary.
- SIGTERM while a file picker or permission dialog was open never exited:
  the dialog's nested loop kept running. SIGTERM now routes through
  `BrowserSession::quit()` (save the session, quit the engine) and a
  three-second watchdog calls `_exit(0)` if the nested loop still holds the
  process.
- The compositor's close request path is exercised directly; it must record
  `last window closed`, not `event loop returned 0`.

## Sanitizer build

The framework can be built with ASAN/UBSAN in a separate tree, and the
frontend supports `ASAN=1` and an overridable `BIN_DIR`:

```bash
cmake -S ../libgui-wayland/serenity/Meta/Lagom -B ../libgui-wayland/build/lagom-asan -GNinja \
    -DBUILD_LAGOM=ON -DENABLE_LAGOM_LIBWEB=ON -DENABLE_LAGOM_LADYBIRD=OFF \
    -DENABLE_CLANG_PLUGINS=OFF -DENABLE_LAGOM_CCACHE=OFF \
    -DENABLE_ADDRESS_SANITIZER=ON -DENABLE_UNDEFINED_SANITIZER=ON \
    -DCMAKE_BUILD_TYPE=Debug
ninja -C ../libgui-wayland/build/lagom-asan LibGfx LibGUI windowserver
make -C UI/LibGUI ASAN=1 \
    LAGOM=../libgui-wayland/build/lagom-asan \
    BIN_DIR=$PWD/build/chrome-asan
ASAN_OPTIONS=detect_leaks=0 scripts/fuzz-frontend.py \
    --chrome build/chrome-asan/Ladybird --seeds 1-5
```

## Limits

- The fake engine does not create shared-memory frames, so page pixels are
  blank; the harness targets the chrome and protocol integration.
- Frame-button coordinates assume the headless Redmond 2000 layout at scale 1.
  The close/resize scenarios fail if the click is not delivered rather than
  silently passing.
- Protocol mutations are hand-seeded; the corpus is generated from the
  handlers in `BrowserSession`, `BrowserWindow`, and `BrowserTab`.
