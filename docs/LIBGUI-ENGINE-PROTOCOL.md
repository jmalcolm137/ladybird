# LibGUI frontend ↔ Ladybird engine protocol

The frontend and the headless engine host are separate processes. They speak a
line-oriented JSON protocol over a Unix socket. Frame pixels never travel over
the socket: the engine publishes a POSIX shared-memory ring per tab and the
frontend maps it.

Version: `Shared/EngineProtocol.h`.

## Frontend → engine

| `type` | Fields | Meaning |
|---|---|---|
| `create_tab` | `request`, `width`, `height`, `url`, `private` | Create a view and a tab. `request` is echoed in `tab_created`. |
| `activate` | `tab` | Make a tab visible and hide the others. |
| `navigate` | `tab`, `url`, `user_input` | Load a URL, or run the URL/search-input path. |
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
| `zoom` | `tab`, `direction` | `in`, `out`, or `reset`. |
| `dialog_response` | `tab`, `kind`, `accept`, `text` | Answer an alert/confirm/prompt. |
| `context_menu_action` | `tab`, `action` | Invoke a menu item from the last `context_menu` event. |
| `permission_response` | `tab`, `request`, `granted` | Answer a page permission request. |
| `file_picker_response` | `tab`, `path` | Answer a file picker request; empty cancels. |
| `select_dropdown_closed` | `tab`, `selected` | Answer a `<select>` popup; omit `selected` when canceled. |
| `download_action` | `action`, `id` | `open`, `show_in_folder`, `pause`, `resume`, `cancel`, or `clear`. |
| `quit` | | Shut the engine host down. |

## Engine → frontend

| `type` | Fields | Meaning |
|---|---|---|
| `hello` | `protocol` | Protocol handshake. |
| `tab_created` | `tab`, `request`, `width`, `height`, `parent` | The view exists; attach the matching frontend tab. `request` matches a frontend request; otherwise `parent` names the tab whose window should receive the engine-created tab. |
| `open_window` | `url`, `private`, `parent` | The page asked for a new native window. The frontend creates it in-process so it shares the engine and profile; `parent` is the requesting tab. |
| `tab_closed` | `tab` | The view is gone. |
| `tab_crashed` | `tab` | The WebContent process died. |
| `url` | `tab`, `url`, `bookmarked` | Active document URL and bookmark state. |
| `title` | `tab`, `title` | Document title. |
| `favicon` | `tab`, `path` or empty | PNG favicon written to the frame directory. |
| `loading` | `tab`, `loading` | Load state. |
| `history` | `tab`, `can_go_back`, `can_go_forward` | History button state. |
| `find_result` | `tab`, `current`, `total` | Current and total find-in-page matches. |
| `link_hover` | `tab`, `url` or empty | Link under the pointer. |
| `cursor` | `tab`, `cursor` | Page cursor name. |
| `dialog` | `tab`, `kind`, `message`, `default` | Page opened a dialog. |
| `clipboard` | `tab`, `text` | Copy/cut result for the frontend clipboard. |
| `fullscreen` | `tab`, `fullscreen` | Page requested/left fullscreen. |
| `bookmarks` | `items` | Root bookmarks (`title`, `url`) for the bookmarks bar. |
| `context_menu` | `tab`, `kind`, `x`, `y`, `items` | Page/link/image/media menu. Items have `text`, `action`, `enabled`, `checkable`, `checked`, or `separator`; `items` on a submenu entry nests. |
| `permission` | `tab`, `request`, `name`, `origin` | Page permission prompt. |
| `file_picker` | `tab`, `multiple`, `filters` | Page opened a file picker. |
| `select_dropdown` | `tab`, `x`, `y`, `min_width`, `items` | Page opened a `<select>` popup. |
| `downloads` | `items` | Initial download list. |
| `download_added` / `download_updated` | `download` | A download was added or changed (`id`, `url`, `destination`, `status`, `downloaded`, `total`, `progress`, `can_resume`, `private`, `error`). |
| `download_removed` | `id`, `private` | A download left the list. |
| `frame_buffer` | `tab`, `name`, `slots`, `slot_size`, `width`, `height` | Map this shared-memory object. |
| `frame` | `tab`, `slot`, `width`, `height`, `stride`, `sequence` | The slot holds a new frame. |

The engine writes into the ring round-robin. The frontend wraps the slot in a
`Gfx::Bitmap` and paints it; no pixel data is serialized.

One engine host serves one frontend process. The frontend may create several
LibGUI windows, each owning a subset of the engine's tabs. Tab-scoped events are
routed by `tab`; `request` and `parent` let the frontend attach engine-created
tabs to the window that asked for them. Windows do not get their own profiles or
engine processes.
