# Ladybird for the Serenity Desktop Environment

A **LibGUI** frontend for the current Ladybird browser. The web engine is the
real Ladybird checkout from `LadybirdBrowser/ladybird`; only the chrome is
replaced. The result is a normal Serenity Desktop Environment application built
against the same LibGUI-for-Wayland (Lagom) stack as File Manager, Terminal, the
Taskbar, and the rest of the SDE.

This is a visual and interaction port, not a fork of the engine. Ladybird keeps
its Skia display-list compositor, its Rust components, its helper processes, and
its third-party dependencies.

## Architecture

```
┌──────────────────────────────┐        control socket        ┌──────────────────────────────┐
│ Ladybird (LibGUI frontend)   │◄────────────────────────────►│ LadybirdEngine               │
│                              │                              │                              │
│ LibGUI-lagom + LibWM         │   frame-ready + slot index   │ Ladybird UI process          │
│ Wayland toplevel             │◄─────────────────────────────│ LibWebView / HeadlessWebView │
│ Renders chrome with Serenity │                              │                              │
│ fonts, theme, widgets        │   POSIX shared-memory ring   │ WebContent / RequestServer / │
│                              │◄─────────────────────────────│ ImageDecoder / Compositor …  │
└──────────────────────────────┘                              └──────────────────────────────┘
```

The two processes exist because the LibGUI Wayland stack and modern Ladybird
have deliberately different C++ foundations:

* The frontend is Serenity userland on Linux: `AK`, `LibCore`, `LibGUI`, `LibGfx`
  software painting, the WindowServer protocol, and LibWM presenting a Wayland
  toplevel. It is the exact stack the SDE uses for every other application.
* The engine host is current Ladybird: `LibWeb`, `LibWebView`, the Rust display
  list, the Skia `Compositor` service, and the multi-process WebContent sandbox.
  It has no LibGUI and no Qt.

The process boundary keeps both halves honest instead of teaching either stack
the other's graphics types. It also preserves Ladybird's process isolation and
crash recovery. The normal frame path is **zero-copy**: the engine sends a
duplicate of the compositor bitmap's anonymous-buffer fd over an inherited
`SOCK_SEQPACKET` channel, and the frontend maps that bitmap read-only and paints
it. The engine defers releasing the compositor backing store until the frontend
acknowledges that it has replaced the frame, so pixels cannot be overwritten
while still on screen. Frames are paced at the output rate (60 Hz by default) and
intermediate paints are coalesced when the frontend is behind.

A **three-slot POSIX shared-memory ring** remains as a fallback for bitmaps
without a shareable fd. In that path the engine copies a completed frame into a
free slot and sends a small JSON message naming it; steady-state copies are
limited to the damaged region, with full copies reserved for first frames,
resizes, and stale slots. Input and navigation are small control messages.

The frontend **does not** contain a web engine. The engine host **does not**
draw chrome. Replacing either half leaves the other intact.

## Building

Requirements are those of both projects:

* Ladybird's Linux build dependencies (see `Documentation/BuildInstructionsLadybird.md`)
* A LibGUI-for-Wayland Lagom build (`../libgui-wayland/build/lagom`)
* The SDE's `WindowServer` only for the headless test, not for normal use

```sh
scripts/build.sh            # engine host, then LibGUI frontend
scripts/run.sh              # run from the build trees
```

`scripts/build-engine.sh` drives `Meta/ladybird.py build --preset Release
LadybirdEngine`. If the host cannot launch Ladybird's sandbox, set
`LADYBIRD_SDE_DISABLE_SANDBOX=1`; the frontend forwards `--disable-sandbox` to
the engine host.

This tree is built with the host's Rust 1.99.0, while upstream pins 1.98.0. Four
`Atomic::fetch_update` calls were renamed to the 1.99 `try_update` spelling; with
a 1.98 toolchain, revert those four one-line changes or use rustup's pinned
toolchain. The engine uses the `libgui` Ladybird profile by default so it
does not contend with a running Qt Ladybird; set `LADYBIRD_SDE_PROFILE=default`
to share the default Ladybird profile, or name another profile. The first build configures Ladybird and builds its helper
processes and resources. `scripts/build-chrome.sh` compiles `UI/LibGUI` against
the LibGUI-lagom tree.

Window decorations, the menubar, and dropdown menus come from the LibGUI/LibWM
stack in `../libgui-wayland`. Menubar menus are xdg popups; LibWM anchors them
to the content top when the menubar is a direct subsurface, and to the menubar
rect when the fallback composed path is used. That logic lives in
`src/WindowServerConnection.cpp` / `src/MenuController.cpp`. Running from this
build tree (`scripts/run.sh`) uses the rebuilt local framework. For an installed
SDE, rebuild the framework and reinstall it into the SDE prefix (see the SDE's
`scripts/install-framework.sh`) so newly started applications pick up the fix.

## Installing into the SDE

```sh
sudo scripts/install-sde.sh                 # /usr/lib/sde-next
SDE_PREFIX=/usr/lib/sde sudo scripts/install-sde.sh
```

The install places:

* `bin/Ladybird` — the LibGUI frontend (plus a `.real` binary and a small
  `LD_LIBRARY_PATH` wrapper)
* `libexec/LadybirdEngine` and Ladybird's helper processes
* `lib/ladybird` — Ladybird's shared libraries
* `share/Lagom` — Ladybird's fonts, themes, and internal pages
* `share/applications/ladybird.desktop` — the Taskbar/menu entry

## Testing without the engine

```sh
scripts/run-headless-test.sh [out.png]
```

This runs the real LibGUI frontend under the SDE's headless `WindowServer`,
using `Tools/fake-engine.py` as a stand-in engine. It exercises the window,
tabs, toolbar, URL bar, menus, shared-memory frame path, and screenshot capture
without waiting for a Ladybird build.

To run the same test with the real engine host after building it:

```sh
LADYBIRD_ENGINE_HOST="$PWD/Build/release/libexec/LadybirdEngine" \
LADYBIRD_TEST_URL="file://$PWD/Tools/test-page.html" \
    scripts/run-headless-test.sh real-engine.png
```

The real-engine run loads the page, renders it in WebContent through Ladybird's
Skia compositor, publishes the frame through shared memory, and captures the
LibGUI window with the page visible. `LADYBIRD_TEST_URL` is optional; without
it the frontend opens `about:newtab`. `Tools/ui-test-page.html` additionally
exercises the select dropdown, file picker, and download UI. This path is
verified in this tree, including clicking a link in the rendered page and
navigating the real engine.

## Implemented in this milestone

* A real SDE LibGUI window with Serenity window decorations, menus, toolbar,
  tab bar, URL bar, status bar, and app icon.
* Tabbed browsing: new/close/activate tabs, titles, favicons, per-tab history
  state, and drag reordering (LibGUI `TabWidget`).
* Navigation: load/search from the URL bar, back, forward, reload (with cache
  bypass support in the protocol), stop, zoom in/out/reset.
* Find in page: Ctrl+F, incremental search, next/previous, and close.
* Bookmarks: Ctrl+D toggle, filled/contour star, a bookmarks bar fed by
  Ladybird's real `BookmarkStore`, and a show/hide action.
* Page dialogs: alert, confirm, and prompt are shown as LibGUI dialogs and
  answered back to the engine.
* Page-initiated fullscreen and F11 window fullscreen.
* Link hover URLs in the status bar, page cursor changes, loading state, and
  window titles.
* Edit menu with undo/redo, cut/copy/paste through the SDE clipboard, select all,
  and find-in-page.
* New window and new private window (`--private`), with the private window's
  tabs created in a private browsing session in the engine. Every normal window
  in one frontend process shares the same engine host and profile, so tabs,
  bookmarks, settings, history, and downloads are shared. A window is only a
  view over its own subset of the engine's tabs.
* Page, link, image, and media context menus. The engine builds the menu model
  (including spelling, text editing, open/copy/download actions); the frontend
  renders it as a native LibGUI menu.
* Tab context menu: new, reload, duplicate, close, and close other tabs.
* Native downloads window with live progress, open/show-in-folder, pause,
  resume, cancel, and clear-completed actions.
* Permission prompts for page requests such as the microphone.
* File picker for `<input type=file>` and native `<select>` dropdowns.
* Find-in-page match count in the find bar.
* A click-to-reload crash overlay for a crashed WebContent process.
* Session restore: every normal window (not just the last one) reopens with its
  previous tabs, size, and active tab on startup, and History → Restore
  Previous Session reopens them on demand. Set
  `LADYBIRD_SDE_DISABLE_SESSION_RESTORE=1` to disable this (the headless test
  does).
* View Source (`Ctrl+U`).
* Internal pages from the Ladybird engine: `about:settings`, `about:history`,
  `about:bookmarks`, `about:downloads`.
* Zero-copy frame delivery through a shared compositor bitmap, with a
  damage-aware shared-memory ring fallback and acknowledged buffer lifetime.

## Parity with the Qt frontend

The Qt frontend remains the reference for behaviour and options. The LibGUI
frontend uses the SerenityOS Browser's chrome layout (a toolbar per tab, a
status bar, and Serenity menus) because that is what "a LibGUI application"
looks like in the SDE, while the engine and the available options come from the
same Ladybird checkout as Qt.

Still to wire to the frontend:

* DevTools, download-history pruning, and the web-compat/report flows
* Bookmark/history management UI beyond the engine's internal pages (the bar
  currently shows root bookmarks only)
* Address-bar autocomplete and search suggestions
* Rich clipboard types, drag and drop, and IME composition
* HiDPI frame scaling (the engine host currently paints at the logical scale)
* GPU/DMA-BUF compositor presentation; the normal CPU path is zero-copy through
  shared bitmaps, while the fallback ring still copies damaged regions
* Settings that need a native dialog rather than the internal settings page

The control protocol is deliberately small and append-only so each of these can
be added without touching the other half.

## Protocol

The control wire format is one JSON object per line over a Unix socket. Frame
pixels normally travel on a separate inherited `SOCK_SEQPACKET` channel as a
shared bitmap fd; the frontend acknowledges replacement through `frame_ack`. The
engine also supports a POSIX shared-memory ring per tab, announced with
`frame_buffer`, for bitmaps that cannot be shared directly. The frontend sends
commands such as `create_tab`, `navigate`, `resize`, `mouse`,
`wheel`, `key`, `find`, `find_next`, `toggle_bookmark`, `context_menu_action`,
`permission_response`, `file_picker_response`, `select_dropdown_closed`,
`download_action`, and `quit`.

The shared constants are in `Shared/EngineProtocol.h`; the protocol version is
checked by the frontend.

## Source map

| Path | Role |
|---|---|
| `UI/LibGUI/` | The LibGUI frontend: window, tabs, engine client, page widget |
| `UI/LadybirdEngine/` | The headless Ladybird UI process and protocol server |
| `Shared/EngineProtocol.h` | Constants shared by both halves |
| `Tools/fake-engine.py` | Protocol/frame test double |
| `scripts/` | Build, run, install, and headless test entry points |
