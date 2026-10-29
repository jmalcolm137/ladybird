# LibGUI frontend UI roadmap

The LibGUI frontend is feature-complete for normal browsing: tabs and multiple
windows, session restore, context menus, find-in-page, zoom, view source,
downloads, permission prompts, file and `<select>` pickers, the engine's
internal pages, and crash/unresponsive-page recovery are all wired.

This document tracks the remaining parity and platform work. It is ordered by
user impact, not by implementation effort. The Qt frontend remains the
reference for behaviour and options.

## Current baseline

- Window and tab chrome, tab close buttons, tab drag-reorder, middle-click
  close, duplicate tab, close other tabs.
- Back/forward/reload/stop, home, new tab/window/private window.
- Address bar with URL entry, search fallback, and autocomplete/search
  suggestions; bookmarks toggle, find bar with match count, zoom controls with
  a visible percentage, view source.
- Bookmarks bar with folders and an overflow menu, downloads window, permission
  prompts, file picker, native `<select>` dropdowns, JavaScript dialogs.
- Reopen-closed-tab and tab search actions; a site indicator/popover with the
  connection state, host, zoom controls, and settings shortcut.
- Session restore with window geometry, active tab, autosave, and a manual
  restore action.
- Crash overlay and unresponsive-page watchdog with click-to-reload.
- Internal `about:settings`, `about:history`, `about:bookmarks`, and
  `about:downloads` pages.

## P1: Daily-use chrome gaps

| Area | Remaining work |
|---|---|
| Address bar | **Done:** autocomplete and search suggestions are exposed over the control socket and shown in a keyboard-navigable popup. Inline completion is still future work. |
| Tabs | **Partially done:** reopen closed tab (`Ctrl+Shift+T`) and tab search are wired. Still missing: pin tab, mute/audio indicator, move tab to a new window, close tabs to the right, and a visible all-tabs overflow strip. |
| Bookmarks | **Partially done:** the bar renders folders and nested menus and has a leading overflow menu. Still missing: chrome-level add/edit/delete/reorder dialogs. `about:bookmarks` already provides management. |
| History | The History menu only has “Show History” and “Restore Previous Session”. Add recent entries, search, and clear actions in the chrome. `about:history` handles deletion. |
| Downloads | The native Downloads window shows private-session downloads and lacks per-session pruning. Filter by the active session and use the engine's pruning action. `about:downloads` already behaves correctly. |
| Site info | **Done (basic):** the address bar shows a Secure/Not secure/Local/Page indicator with a menu for host, zoom, and settings. Certificate details, permission summaries, and per-site settings are still future work. |
| Find | Add a match-case toggle (the engine already supports case sensitivity). |
| Zoom | **Done:** the engine emits `zoom_changed` and the toolbar shows the current percentage with zoom controls. |
| File picker | Support multiple files and pass the page's filters to the picker. |
| Settings | Native dialogs only where `about:settings` cannot express the choice; this is low priority. |

## P2: Platform integration

- **HiDPI**: the engine host paints at logical scale. Add device-pixel-ratio
  information to the frame protocol and scale in the frontend.
- **IME composition**: forward composition events, not only key events.
- **Drag and drop**: links, files, tabs, and text.
- **Rich clipboard**: **Done:** WebContent's rich copy path, text/HTML and
  image representations, and page-initiated clipboard reads. The SDE clipboard
  stores one MIME type per item, so other applications receive the best single
  representation (plain text for rich text), while the browser keeps the full
  item for pasting back into a page. File lists still need the portal to carry
  multiple types.
- **Accessibility**: expose web-content accessibility through the LibGUI/AT-SPI
  bridge.

## P3: Diagnostics and advanced features

- **DevTools**: **Done:** the engine host runs the upstream DevTools server,
  the frontend exposes Tools → Inspect, `Ctrl+Shift+I`/`F12`, `--devtools[=port]`,
  and a port banner; the client is the upstream Firefox client.
- Web compatibility report flow
- Crash report submission UI
- Process manager / performance monitor
- Picture-in-picture controls
- Per-site permission memory and management
- Native settings dialogs for the few settings that need them

## Shelved

GPU/zero-copy frame sharing. The stable rendering path is the acknowledged
shared-memory ring with damage-only copies. Direct bitmap sharing and frame
pacing previously caused scroll and freeze regressions, so they stay off until a
design is proven not to destabilize the UI. Any future work must be gated by
scroll, resize, multi-window, and session-restore tests.

## Engine-host parity

- **Native session restore:** the engine host now uses `SessionStore` /
  `SessionHistorySnapshot` for startup recovery, including full per-tab
  history. Restored background tabs are lazy and are registered with the
  native store even before they load, so they survive the next launch.
- **Active view:** `EngineApplication` exposes the active `ViewImplementation`
  to the shared `Application` actions and session machinery.
- **Navigation ordering:** `EngineServer` queues a navigation while the current
  load is committing, preventing UI/WebContent session-history races.
- **Headless view:** `HeadlessWebView` is upstream Ladybird's native headless
  view, not a custom replacement. The engine host cannot use a GUI
  `WebContentView` because it has no native window; the remaining native
  lifecycle responsibilities live in `EngineServer`.
- **Regression harness:** `scripts/compare-frontends.sh [url]` compares the
  upstream Qt and LibGUI frontends headlessly and fails on a crash signature.

## Deployment gaps

- `/usr/lib/sde-next` still contains the older `libgui-wayland` framework, so
  normal launches do not get the latest menu-anchor, modifier-state, and scroll
  fixes. Rebuild/reinstall it.
- The Ladybird `.af` file and Taskbar entry are still manual; `scripts/install-sde.sh`
  should install them.
- **Done:** the engine host watches the resolver configuration files and
  restarts the RequestServer processes when `/etc/resolv.conf` (or the other
  resolver files) is replaced. This is a local safeguard for the upstream
  Landlock bug in Ladybird issue #13407. `LADYBIRD_SDE_RESOLVER_FILES` can
  override the watched paths for testing.

## Suggested order

1. ~~Reopen closed tab and tab search~~ (done).
2. ~~Address-bar autocomplete and search suggestions~~ (done).
3. ~~Bookmark folders/overflow and the site info popover~~ (done; folder
   management dialogs remain).
4. ~~Zoom percentage and address-bar polish~~ (done).
5. Downloads private filtering/pruning, find match case, file-picker filters.
6. HiDPI, IME, and drag and drop.
7. The remaining diagnostics (compatibility report, crash submission UI, process monitor).
