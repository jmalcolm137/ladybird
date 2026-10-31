# LibGUI frontend UI roadmap

The LibGUI frontend is feature-complete for normal browsing: tabs and multiple
windows, session restore, context menus, find-in-page, zoom, view source,
downloads, permission prompts, file and `<select>` pickers, the engine's
internal pages, and crash/unresponsive-page recovery are all wired.

This document tracks the remaining parity and platform work. It is ordered by
user impact, not by implementation effort. The Qt frontend remains the
reference for behaviour and options.

## Current baseline

Toolbar layout rule: the site indicator is always immediately to the left of
the address bar; window-level controls (the tab list) sit to its left. Menus
opened from toolbar buttons are separate menu objects from the menubar menus.
The tab-list and site indicators are icon-only with tooltips, using the same
glyphs as Qt: a chevron ("Show All Tabs") and the site-settings sliders
("Site settings", a warning triangle and "Not secure" on HTTP, and "Site
information" on local or browser pages).

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
- Session restore with window geometry, active tab, and autosave.
- Crash overlay and unresponsive-page watchdog with click-to-reload.
- Internal `about:settings`, `about:history`, `about:bookmarks`, and
  `about:downloads` pages.

## P1: Daily-use chrome gaps

| Area | Remaining work |
|---|---|
| Address bar | **Done:** autocomplete and search suggestions are exposed over the control socket and shown in a keyboard-navigable popup. Inline completion is still future work. |
| Tabs | **Done:** reopen closed tab, tab search, pin tab, mute/audio indicators on the tab, move tab to a new window, close tabs to the right, and a Tabs menu plus toolbar button. |
| Bookmarks | **Done:** the bar renders folders and nested menus; the chrome can add, edit, remove, and move bookmarks, and create and rename folders in a manager window. |
| History | **Done:** the History menu lists recent entries, searches titles and URLs into a native results window, clears history after confirmation, and links to `about:history`. |
| Downloads | **Done:** the window filters to the active window's session and Clear Completed prunes only that session. |
| Site info | **Done:** the indicator shows host, security state, zoom, site settings, the permission decisions made for the site, and a Forget action. HTTPS pages can open a certificate view with subject, issuer, validity, serial, SHA-256 fingerprint, TLS version, and cipher. Remembered permission decisions are persisted per origin in the profile and applied without prompting; private windows neither use nor store them. |
| Find | **Done:** the find bar has a match-case toggle, highlights all matches, and closes on Escape. |
| Zoom | **Done:** the engine emits `zoom_changed` and the toolbar shows the current percentage with zoom controls. |
| File picker | **Partially done:** the page's filters reach the picker. Multiple selection needs a multi-select LibGUI file picker; the engine and protocol already accept a list. |
| Settings | Native dialogs only where `about:settings` cannot express the choice; this is low priority. |

## P2: Platform integration

- **HiDPI**: **Done.** The frontend sends the LibGUI backing-store scale with
  each tab, the engine paints at that device-pixel ratio, and frames travel as
  physical pixels with their intrinsic scale so the frontend presents them at
  logical size. Mouse, context-menu, and `<select>` positions convert between
  logical and device pixels at the boundary.
- **IME composition**: **Done.** LibWM implements text-input-v3, the frontend
  enables it only while an editable field is focused, and the engine forwards
  marked and committed text to WebContent. LibGUI text fields receive committed
  text; their preedit is shown by the input method's own candidate window.
- **Drag and drop**: **Mostly done.** External text, URL, and file drags are
  forwarded to WebContent as real `DragEvent`s and reach page drop targets.
  Dragging a link, selection, file, or tab *out* of the page/window still needs
  a LibGUI drag-source API; tab reordering inside a window is already supported.
- **Rich clipboard**: **Done:** WebContent's rich copy path, text/HTML and
  image representations, and page-initiated clipboard reads. The LibWM
  clipboard portal now carries a list of representations per item, so
  text/plain, text/html, and images are all available to other applications at
  the same time.
- **Accessibility**: expose web-content accessibility through the LibGUI/AT-SPI
  bridge.

## P3: Diagnostics and advanced features

- **DevTools**: **Done:** the engine host runs the upstream DevTools server,
  the frontend exposes Tools → Enable/Open DevTools, `Ctrl+Shift+I`/`F12`, `--devtools[=port]`,
  and a port banner; the client is the upstream Firefox client.
- **Web compatibility report flow: Done.** Help → Report Website Issue…
  captures the page URL, user agent, viewport, DPR, zoom, build details, a
  screenshot, and optional layout/stacking/display-list dumps into a native
  review window. It validates the description, shows the diagnostics, streams
  send progress, auto-retries transient failures, and offers a manual Retry
  after a terminal failure.
- Crash report submission UI
- Process manager / performance monitor
- Picture-in-picture controls
- Per-site permission management UI beyond the site menu's Forget action
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

- ~~`/usr/lib/sde-next` still contains the older `libgui-wayland` framework~~
  **Done:** the framework and SDE binaries were rebuilt and reinstalled into
  `/usr/lib/sde-next` (runtime, headers, and source). Installed TextEditor and
  Terminal were verified with the multi-format clipboard, menu anchoring,
  modifier state, and wheel scrolling. Note that running apps keep the
  libraries they mapped; new launches pick up the new framework.
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
6. ~~HiDPI, IME, and drag and drop~~ (done, except dragging page content out).
7. The remaining diagnostics (crash submission UI, process monitor) and accessibility.
