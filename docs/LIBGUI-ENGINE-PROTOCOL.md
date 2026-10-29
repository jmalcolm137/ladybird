# LibGUI frontend ↔ Ladybird engine protocol

The frontend and the headless engine host are separate processes. They speak a
line-oriented JSON protocol over a Unix socket. Frame pixels never travel over
the socket: the engine publishes a POSIX shared-memory ring per tab and the
frontend maps it.

Version: 8. `Shared/EngineProtocol.h` is authoritative.

## Frontend → engine

| `type` | Fields | Meaning |
|---|---|---|
| `create_tab` | `request`, `width`, `height`, `url`, `private`, `window`, `session_tab`, `session_window` | Create a view and a tab. `request` is echoed in `tab_created`; `window` associates the tab with a frontend window. `session_tab`/`session_window` identify a natively restored tab/window so its session history can be restored from the engine's `SessionStore`. |
| `activate` | `tab`, `window` | Make this tab the active tab of `window`; the active tab of every other window stays visible. |
| `navigate` | `tab`, `url`, `user_input` | Load a URL, or run the URL/search-input path. |
| `autocomplete` | `tab`, `request`, `query` | Query history/bookmarks/search suggestions. `request` is echoed in `autocomplete_results`; an empty query cancels. |
| `back` / `forward` | `tab` | Traverse session history. |
| `reload` | `tab`, `bypass_cache` | Reload. |
| `stop` | `tab` | Stop loading. |
| `resize` | `tab`, `width`, `height` | Set the page viewport. |
| `focus` | `tab`, `focused` | Tell the page whether it has system focus. |
| `mouse` | `tab`, `event`, `x`, `y`, `button`, `buttons`, `ctrl`, `shift`, `alt`, `super`, `click_count` | Pointer event. |
| `wheel` | `tab`, `x`, `y`, `delta_x`, `delta_y`, `ctrl`, `shift`, `alt`, `super` | Wheel/touchpad scroll in pixels. |
| `key` | `tab`, `event`, `key`, `codepoint`, `ctrl`, `shift`, `alt`, `super`, `repeat` | Keyboard event; `key` is the Serenity key name. |
| `find` | `tab`, `query` | Start/replace find-in-page; empty ends it. |
| `find_next` / `find_previous` | `tab` | Move between matches. |
| `toggle_bookmark` | `tab` | Toggle the current URL in the bookmark store. |
| `copy` / `cut` | `tab` | Put the page selection on the clipboard. |
| `paste` | `tab`, `text` | Paste text into the page. |
| `select_all`, `undo`, `redo` | `tab` | Editing commands. |
| `zoom` | `tab`, `direction` | `in`, `out`, or `reset`. The engine answers with `zoom_changed`. |
| `dialog_response` | `tab`, `kind`, `accept`, `text` | Answer an alert/confirm/prompt. |
| `context_menu_action` | `tab`, `action` | Invoke a menu item from the last `context_menu` event. |
| `permission_response` | `tab`, `request`, `granted` | Answer a page permission request. |
| `file_picker_response` | `tab`, `path` | Answer a file picker request; empty cancels. |
| `select_dropdown_closed` | `tab`, `selected` | Answer a `<select>` popup; omit `selected` when canceled. |
| `frame_ack` | `tab`, `sequence` | The frontend has replaced the frame it was displaying. The engine may reuse that shared-memory slot. |
| `download_action` | `action`, `id` | `open`, `show_in_folder`, `pause`, `resume`, `cancel`, or `clear`. |
| `register_session_tab` | `session_window`, `request`, `window`, `url` | Register a lazily restored tab in the native session store without creating a view. The engine answers with `session_tab_registered`. |
| `window_geometry` | `session_window`, `x`, `y`, `width`, `height`, `maximized` | Persist a frontend window's geometry in the native session store. |
| `close_window` | `session_window`, `active` | Mark a frontend window closed in the native session store. |
| `quit` | | Shut the engine host down. |

## Engine → frontend

| `type` | Fields | Meaning |
|---|---|---|
| `hello` | `protocol` | Protocol handshake. |
| `session_plan` | `windows` | Native startup-recovery plan. Each window has `window`, `active`, optional geometry, and `tabs`; each tab has `session_tab`, `url`, and `pinned`. Sent once after `hello`. An empty list means open the default window. |
| `session_window` | `window`, `session_window` | The engine assigned a native session-window ID to a frontend window. The frontend stores it for geometry and close reporting. |
| `session_tab_registered` | `request`, `window`, `session_tab` | The native session store assigned an ID to a lazily restored tab. |
| `tab_created` | `tab`, `request`, `width`, `height`, `parent`, `window` | The view exists; attach the matching frontend tab. `request` matches a frontend request; otherwise `parent`/`window` name the owning window. |
| `open_window` | `url`, `private`, `parent` | The page asked for a new native window. The frontend creates it in-process so it shares the engine and profile; `parent` is the requesting tab. |
| `tab_closed` | `tab` | The view is gone. |
| `tab_crashed` | `tab` | The WebContent process died. |
| `tab_unresponsive` | `tab`, `unresponsive` | The renderer is spinning without presenting frames after input; the frontend offers a reload. A later frame clears it. |
| `url` | `tab`, `url`, `bookmarked` | Active document URL and bookmark state. |
| `title` | `tab`, `title` | Document title. |
| `favicon` | `tab`, `path` or empty | PNG favicon written to the frame directory. |
| `loading` | `tab`, `loading` | Load state. |
| `history` | `tab`, `can_go_back`, `can_go_forward` | History button state. |
| `find_result` | `tab`, `current`, `total` | Current and total find-in-page matches. |
| `zoom_changed` | `tab`, `zoom` | The page zoom level changed. |
| `autocomplete_results` | `tab`, `request`, `final`, `items` | Suggestions for an `autocomplete` query. Items have `text` (the value to navigate to), `display` (the URL or query to show), optional `title` and `subtitle`, `source`, and `search`. `final` marks the last update for the query. |
| `link_hover` | `tab`, `url` or empty | Link under the pointer. |
| `cursor` | `tab`, `cursor` | Page cursor name. |
| `dialog` | `tab`, `kind`, `message`, `default` | Page opened a dialog. |
| `clipboard` | `tab`, `text` | Copy/cut result for the frontend clipboard. |
| `fullscreen` | `tab`, `fullscreen` | Page requested/left fullscreen. |
| `bookmarks` | `items` | The bookmark tree for the bookmarks bar. Each item has `id`, `type` (`bookmark` or `folder`), `title`, and either `url` or nested `items`. |
| `context_menu` | `tab`, `kind`, `x`, `y`, `items` | Page/link/image/media menu. Items have `text`, `action`, `enabled`, `checkable`, `checked`, or `separator`; `items` on a submenu entry nests. |
| `permission` | `tab`, `request`, `name`, `origin` | Page permission prompt. |
| `file_picker` | `tab`, `multiple`, `filters` | Page opened a file picker. |
| `select_dropdown` | `tab`, `x`, `y`, `min_width`, `items` | Page opened a `<select>` popup. |
| `downloads` | `items` | Initial download list. |
| `download_added` / `download_updated` | `download` | A download was added or changed (`id`, `url`, `destination`, `status`, `downloaded`, `total`, `progress`, `can_resume`, `private`, `error`). |
| `download_removed` | `id`, `private` | A download left the list. |
| `frame_buffer` | `tab`, `name`, `slots`, `slot_size`, `width`, `height` | Map this shared-memory object. |
| `frame` | `tab`, `slot`, `width`, `height`, `stride`, `sequence`, `full`, `damage_x`, `damage_y`, `damage_width`, `damage_height` | The slot holds a new frame. `full` means the whole slot was rewritten; otherwise only the damage rectangle changed. |

The engine writes into a free slot in the ring. The frontend wraps the slot in a
`Gfx::Bitmap` and paints it; no pixel data is serialized. A slot stays in flight
until the frontend sends `frame_ack`, so the engine never overwrites pixels the
frontend may still display. When all slots are in flight, the engine coalesces
new paints and publishes only the latest one after an acknowledgement. In steady
state it copies just the damaged region into a slot whose contents are an older
frame; a full copy is used for the first frame, after a resize, or when the
retained damage history can no longer describe every changed pixel.

One engine host serves one frontend process. The frontend may create several
LibGUI windows, each owning a subset of the engine's tabs. Tab-scoped events are
routed by `tab`; `request` and `parent` let the frontend attach engine-created
tabs to the window that asked for them. Windows do not get their own profiles or
engine processes.
