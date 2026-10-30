/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineServer.h"
#include "EngineApplication.h"

#include <AK/Array.h>
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebCommon/HTML/AudioPlayState.h>
#include <LibWebCommon/HTML/WebViewHints.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Menu.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/ViewImplementation.h>
#include <Shared/EngineProtocol.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace LadybirdSDE {

static JsonArray clipboard_item_to_json(Web::Clipboard::SystemClipboardItem const&);

// A headless view for a top-level tab. The base class turns links opened in a
// new tab or window into child views that no chrome could see; the engine host
// wants those requests to go through the application handlers instead, so the
// frontend can create a real tab or window.
class EngineHeadlessWebView final : public WebView::HeadlessWebView {
    AK_ALLOC_WITH_KMALLOC;

public:
    static NonnullOwnPtr<EngineHeadlessWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, WebView::IsPrivate is_private)
    {
        auto view = adopt_own(*new EngineHeadlessWebView(move(theme), viewport_size, is_private));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

protected:
    EngineHeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, WebView::IsPrivate is_private)
        : HeadlessWebView(move(theme), viewport_size, is_private)
    {
    }

    virtual WebView::ViewImplementation* create_view_for_new_tab_or_window(WebView::IsPrivate) override { return nullptr; }
};

EngineServer::EngineServer(NonnullOwnPtr<Core::LocalSocket> socket, ByteString frame_directory, Core::AnonymousBuffer theme)
    : m_socket(move(socket))
    , m_frame_directory(move(frame_directory))
    , m_theme(move(theme))
{
}

EngineServer::~EngineServer()
{
    EngineApplication::the().set_open_blank_new_tab_handler({});
    EngineApplication::the().set_open_new_window_handler({});
    EngineApplication::the().set_active_web_view_provider({});
    EngineApplication::the().set_clipboard_write_handler({});
    EngineApplication::the().set_devtools_event_handler({});
    if (m_unresponsive_timer)
        m_unresponsive_timer->stop();
    if (m_resolver_watch_timer)
        m_resolver_watch_timer->stop();
    if (m_write_notifier) {
        m_write_notifier->close();
        m_write_notifier.clear();
    }
    for (auto& [id, tab] : m_tabs) {
        destroy_frame_buffer(*tab);
        auto favicon_path = ByteString::formatted("{}/favicon-{}.png", m_frame_directory, id);
        (void)Core::System::unlink(favicon_path.view());
    }
}

ErrorOr<void> EngineServer::start()
{
    // Commands are read in a loop until the socket would block, so it must not block. A blocking socket would also
    // stop this UI process from servicing WebContent IPC while it waits for the next chrome command.
    TRY(m_socket->set_blocking(false));

    m_socket->on_ready_to_read = [this] {
        read_available();
    };

    if (auto fd = m_socket->fd(); fd.has_value()) {
        m_write_notifier = Core::Notifier::construct(*fd, Core::Notifier::Type::Write);
        m_write_notifier->on_activation = [this] {
            flush_writes();
        };
        m_write_notifier->set_enabled(false);
    }

    m_unresponsive_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1000), [this] {
        check_unresponsive_tabs();
    });
    m_unresponsive_timer->start();

    // Landlock pins /etc/resolv.conf by inode. NetworkManager replaces the file
    // when the network changes, which leaves the running RequestServers unable
    // to read the new resolver configuration. Watch the files and ask the
    // manager to relaunch the helpers before the next navigation.
    snapshot_resolver_files();
    m_resolver_watch_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(2000), [this] {
        check_resolver_files();
    });
    m_resolver_watch_timer->start();

    JsonObject hello;
    hello.set("type"sv, "hello"sv);
    hello.set("protocol"sv, static_cast<unsigned>(protocol_version));
    send_event(move(hello));

    // DevTools may already be running: the frontend asked the engine to start it with --devtools, before the
    // control socket existed. Tell the chrome now so it can show the banner.
    if (auto port = EngineApplication::the().devtools_port_if_enabled(); port.has_value()) {
        JsonObject event;
        event.set("type"sv, "devtools_enabled"sv);
        event.set("port"sv, static_cast<u64>(*port));
        send_event(move(event));
    }

    EngineApplication::the().set_open_new_window_handler([this](URL::URL const& url, WebView::IsPrivate is_private) {
        JsonObject event;
        event.set("type"sv, "open_window"sv);
        event.set("url"sv, url.to_string());
        event.set("private"sv, is_private == WebView::IsPrivate::Yes);
        event.set("parent"sv, m_focused_tab_id != 0 ? m_focused_tab_id : m_active_tab_id);
        send_event(move(event));
    });

    EngineApplication::the().set_active_web_view_provider([this]() -> Optional<WebView::ViewImplementation&> {
        if (auto* tab = tab_for_id(m_active_tab_id))
            return static_cast<WebView::ViewImplementation&>(*tab->view);
        return {};
    });

    EngineApplication::the().set_clipboard_write_handler([this](Web::Clipboard::SystemClipboardItem const& item) {
        auto items = clipboard_item_to_json(item);
        if (items.is_empty())
            return;

        JsonObject event;
        event.set("type"sv, "clipboard_write"sv);
        event.set("items"sv, move(items));
        send_event(move(event));
    });

    EngineApplication::the().set_devtools_event_handler([this](bool enabled, u16 port) {
        JsonObject event;
        event.set("type"sv, enabled ? "devtools_enabled"sv : "devtools_disabled"sv);
        if (enabled)
            event.set("port"sv, static_cast<u64>(port));
        send_event(move(event));
    });

    EngineApplication::the().set_open_blank_new_tab_handler([this](Web::HTML::ActivateTab) -> Optional<WebView::ViewImplementation&> {
        auto viewport_size = Gfx::IntSize { 1024, 768 };
        auto is_private = WebView::IsPrivate::No;
        auto parent_id = m_focused_tab_id != 0 ? m_focused_tab_id : m_active_tab_id;
        u64 window_id = 0;
        if (auto* active = tab_for_id(parent_id)) {
            viewport_size = active->viewport_size;
            is_private = active->view->is_private();
            window_id = active->window_id;
        }
        auto& tab = create_tab(viewport_size, is_private == WebView::IsPrivate::Yes, 0, parent_id, window_id);
        return static_cast<WebView::ViewImplementation&>(*tab.view);
    });

    send_session_plan();
    send_bookmarks();
    send_downloads();
    return {};
}

void EngineServer::quit(int status)
{
    if (m_quitting)
        return;
    m_quitting = true;
    Core::EventLoop::current().quit(status);
}

void EngineServer::read_available()
{
    Array<u8, 64 * 1024> buffer;

    while (true) {
        auto result = m_socket->read_some(buffer.span());
        if (result.is_error()) {
            if (result.error().is_errno() && result.error().code() == EAGAIN)
                return;
            warnln("LadybirdEngine: socket read failed: {}", result.error());
            quit();
            return;
        }

        auto bytes = result.release_value();
        if (bytes.is_empty()) {
            // The chrome closed the connection; nothing left to serve.
            quit();
            return;
        }

        m_read_buffer.append(bytes.data(), bytes.size());

        while (true) {
            auto newline_index = m_read_buffer.find_first_index('\n');
            if (!newline_index.has_value())
                break;

            auto line_length = *newline_index;
            auto line_view = StringView { reinterpret_cast<char const*>(m_read_buffer.data()), line_length };
            if (!line_view.is_empty() && line_view.ends_with('\r'))
                line_view = line_view.substring_view(0, line_view.length() - 1);

            // Remove the command from the buffer before dispatching it. Some
            // commands can run nested event loops, during which the socket
            // notifier can call read_available again.
            auto line = String::from_utf8(line_view);
            m_read_buffer.remove(0, line_length + 1);
            if (line.is_error()) {
                warnln("LadybirdEngine: could not decode command: {}", line.error());
                continue;
            }
            dispatch_line(line.value().bytes_as_string_view());
        }

        if (m_read_buffer.size() > 4 * 1024 * 1024) {
            warnln("LadybirdEngine: chrome sent an oversized command; disconnecting");
            quit();
            return;
        }
    }
}

void EngineServer::dispatch_line(StringView line)
{
    auto parsed = JsonParser::parse(line);
    if (parsed.is_error()) {
        warnln("LadybirdEngine: could not parse command: {}", parsed.error());
        return;
    }

    auto value = parsed.release_value();
    if (!value.is_object()) {
        warnln("LadybirdEngine: command was not a JSON object");
        return;
    }

    dispatch_command(value.as_object());
}

void EngineServer::dispatch_command(JsonObject const& command)
{
    auto type = command.get_string("type"sv);
    if (!type.has_value()) {
        warnln("LadybirdEngine: command has no type");
        return;
    }

    if (*type == "create_tab"sv) {
        handle_create_tab(command);
    } else if (*type == "quit"sv) {
        quit();
    } else if (*type == "download_action"sv) {
        handle_download_action(command);
    } else if (*type == "history_recent"sv) {
        handle_history_recent(command);
    } else if (*type == "history_search"sv) {
        handle_history_search(command);
    } else if (*type == "history_clear"sv) {
        handle_history_clear();
    } else if (*type == "add_bookmark"sv) {
        handle_add_bookmark(command);
    } else if (*type == "edit_bookmark"sv) {
        handle_edit_bookmark(command);
    } else if (*type == "remove_bookmark"sv) {
        handle_remove_bookmark(command);
    } else if (*type == "add_folder"sv) {
        handle_add_folder(command);
    } else if (*type == "edit_folder"sv) {
        handle_edit_folder(command);
    } else if (*type == "move_bookmark"sv) {
        handle_move_bookmark(command);
    } else if (*type == "clipboard_changed"sv) {
        handle_clipboard_changed(command);
    } else if (*type == "toggle_devtools"sv) {
        handle_toggle_devtools();
    } else if (*type == "open_devtools_client"sv) {
        handle_open_devtools_client();
    } else if (*type == "close_window"sv) {
        handle_close_window(command);
    } else if (*type == "window_geometry"sv) {
        handle_window_geometry(command);
    } else if (*type == "register_session_tab"sv) {
        handle_register_session_tab(command);
    } else {
        auto tab_id = command.get_u64("tab"sv);
        if (!tab_id.has_value()) {
            warnln("LadybirdEngine: '{}' command has no tab", *type);
            return;
        }

        if (*type == "activate"sv) {
            handle_activate_tab(*tab_id, command);
        } else if (*type == "navigate"sv) {
            handle_navigate(*tab_id, command);
        } else if (*type == "autocomplete"sv) {
            handle_autocomplete(*tab_id, command);
        } else if (*type == "back"sv) {
            if (auto* tab = tab_for_id(*tab_id))
                tab->view->traverse_the_history_by_delta(-1);
        } else if (*type == "forward"sv) {
            if (auto* tab = tab_for_id(*tab_id))
                tab->view->traverse_the_history_by_delta(+1);
        } else if (*type == "reload"sv) {
            if (auto* tab = tab_for_id(*tab_id))
                tab->view->reload(command.get_bool("bypass_cache"sv).value_or(false) ? WebView::BypassCache::Yes : WebView::BypassCache::No);
        } else if (*type == "stop"sv) {
            if (auto* tab = tab_for_id(*tab_id))
                tab->view->stop_loading();
        } else if (*type == "resize"sv) {
            handle_resize(*tab_id, command);
        } else if (*type == "focus"sv) {
            handle_focus(*tab_id, command);
        } else if (*type == "mouse"sv) {
            handle_mouse(*tab_id, command);
        } else if (*type == "key"sv) {
            handle_key(*tab_id, command);
        } else if (*type == "wheel"sv) {
            handle_wheel(*tab_id, command);
        } else if (*type == "dialog_response"sv) {
            handle_dialog_response(*tab_id, command);
        } else if (*type == "close_tab"sv) {
            handle_close_tab(*tab_id);
        } else if (*type == "zoom"sv) {
            handle_zoom(*tab_id, command);
        } else if (*type == "find"sv) {
            handle_find(*tab_id, command);
        } else if (*type == "find_next"sv) {
            handle_find_next(*tab_id, true);
        } else if (*type == "find_previous"sv) {
            handle_find_next(*tab_id, false);
        } else if (*type == "toggle_bookmark"sv) {
            handle_toggle_bookmark(*tab_id);
        } else if (*type == "pin_tab"sv) {
            handle_pin_tab(*tab_id, command);
        } else if (*type == "toggle_mute"sv) {
            handle_toggle_mute(*tab_id);
        } else if (*type == "certificate_info"sv) {
            handle_certificate_info(*tab_id, command);
        } else if (*type == "ime_marked_text"sv) {
            handle_ime_marked_text(*tab_id, command);
        } else if (*type == "ime_commit_text"sv) {
            handle_ime_commit_text(*tab_id, command);
        } else if (*type == "ime_unmark_text"sv) {
            handle_ime_unmark_text(*tab_id);
        } else if (*type == "copy"sv) {
            handle_clipboard_copy(*tab_id, false);
        } else if (*type == "cut"sv) {
            handle_clipboard_copy(*tab_id, true);
        } else if (*type == "paste"sv) {
            handle_paste(*tab_id, command);
        } else if (*type == "select_all"sv) {
            handle_select_all(*tab_id);
        } else if (*type == "undo"sv) {
            handle_undo_redo(*tab_id, false);
        } else if (*type == "redo"sv) {
            handle_undo_redo(*tab_id, true);
        } else if (*type == "context_menu_action"sv) {
            handle_context_menu_action(*tab_id, command);
        } else if (*type == "permission_response"sv) {
            handle_permission_response(*tab_id, command);
        } else if (*type == "file_picker_response"sv) {
            handle_file_picker_response(*tab_id, command);
        } else if (*type == "select_dropdown_closed"sv) {
            handle_select_dropdown_closed(*tab_id, command);
        } else if (*type == "frame_ack"sv) {
            handle_frame_ack(*tab_id, command);
        } else {
            warnln("LadybirdEngine: unknown command '{}'", *type);
        }
    }
}

EngineServer::Tab* EngineServer::tab_for_id(u64 tab_id)
{
    auto it = m_tabs.find(tab_id);
    if (it == m_tabs.end())
        return nullptr;
    return it->value.ptr();
}

EngineServer::Tab& EngineServer::create_tab(Gfx::IntSize viewport_size, bool is_private, u64 request_id, u64 parent, u64 window_id, int device_pixel_ratio)
{
    auto id = m_next_tab_id++;
    auto view = EngineHeadlessWebView::create(m_theme, viewport_size.to_type<Web::DevicePixels>(), is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);

    m_tabs.set(id, make<Tab>(id, request_id, move(view), viewport_size));
    auto& tab_ref = *m_tabs.find(id)->value;
    tab_ref.parent = parent;
    tab_ref.window_id = window_id;
    tab_ref.device_pixel_ratio = device_pixel_ratio > 0 ? device_pixel_ratio : 1;
    tab_ref.view->set_device_pixel_ratio_for_testing(tab_ref.view->page(), tab_ref.device_pixel_ratio);
    tab_ref.view->reset_viewport_size(viewport_size.to_type<Web::DevicePixels>());
    wire_autocomplete(tab_ref);
    wire_tab(tab_ref);

    m_active_tab_id = id;
    if (window_id != 0 && !m_window_active_tabs.contains(window_id))
        m_window_active_tabs.set(window_id, id);
    send_tab_created(tab_ref);
    ensure_frame_buffer(tab_ref, tab_ref.viewport_size);
    on_view_painted(tab_ref);
    return tab_ref;
}

EngineServer::Tab& EngineServer::create_child_tab(Tab& parent, WebView::CanonicalTraversable& traversable)
{
    auto id = m_next_tab_id++;
    auto view = WebView::HeadlessWebView::create_child(*parent.view, traversable);

    m_tabs.set(id, make<Tab>(id, 0, move(view), parent.viewport_size));
    auto& tab_ref = *m_tabs.find(id)->value;
    tab_ref.parent = parent.id;
    tab_ref.window_id = parent.window_id;
    tab_ref.device_pixel_ratio = parent.device_pixel_ratio;
    wire_autocomplete(tab_ref);
    wire_tab(tab_ref);

    send_tab_created(tab_ref);
    ensure_frame_buffer(tab_ref, tab_ref.viewport_size);
    on_view_painted(tab_ref);
    send_url(tab_ref);
    send_title(tab_ref);
    send_loading_state(tab_ref, tab_ref.view->is_loading());
    send_audio_state(tab_ref);
    return tab_ref;
}

void EngineServer::wire_autocomplete(Tab& tab)
{
    auto is_private = tab.view->is_private() == WebView::IsPrivate::Yes;
    tab.autocomplete = new WebView::Autocomplete(is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);

    auto tab_id = tab.id;
    tab.autocomplete->on_autocomplete_query_complete = [this, tab_id](WebView::AutocompleteQueryID query_id, Vector<WebView::AutocompleteSuggestion> suggestions, WebView::AutocompleteResultKind kind) {
        send_autocomplete_results(tab_id, query_id, move(suggestions), kind);
    };
}

void EngineServer::wire_tab(Tab& tab)
{
    auto tab_id = tab.id;

    tab.view->on_ready_to_paint = [this, tab_id] {
        if (auto* tab = tab_for_id(tab_id))
            on_view_painted(*tab);
    };

    tab.view->on_title_change = [this, tab_id](Utf16String const&) {
        if (auto* tab = tab_for_id(tab_id))
            send_title(*tab);
    };

    tab.view->on_url_change = [this, tab_id](URL::URL const&) {
        if (auto* tab = tab_for_id(tab_id)) {
            send_url(*tab);
            send_history_state(*tab);
        }
    };

    tab.view->on_load_start = [this, tab_id] {
        if (auto* tab = tab_for_id(tab_id)) {
            send_loading_state(*tab, true);
            send_url(*tab);
        }
    };

    tab.view->on_load_finish = [this, tab_id](URL::URL const&) {
        if (auto* tab = tab_for_id(tab_id)) {
            send_loading_state(*tab, false);
            send_url(*tab);
            send_history_state(*tab);
            flush_pending_navigation(*tab);
        }
    };

    tab.view->on_loading_state_change = [this, tab_id](bool loading) {
        if (auto* tab = tab_for_id(tab_id))
            send_loading_state(*tab, loading);
    };

    tab.view->on_audio_play_state_changed = [this, tab_id](Web::HTML::AudioPlayState) {
        if (auto* tab = tab_for_id(tab_id))
            send_audio_state(*tab);
    };

    tab.view->on_cursor_change = [this, tab_id](Gfx::Cursor const& cursor) {
        if (auto* tab = tab_for_id(tab_id))
            send_cursor(*tab, cursor);
    };

    tab.view->on_link_hover = [this, tab_id](URL::URL const& url) {
        if (tab_for_id(tab_id)) {
            JsonObject event;
            event.set("type"sv, "link_hover"sv);
            event.set("tab"sv, tab_id);
            event.set("url"sv, url.to_string());
            send_event(move(event));
        }
    };

    tab.view->on_link_unhover = [this, tab_id] {
        if (tab_for_id(tab_id)) {
            JsonObject event;
            event.set("type"sv, "link_hover"sv);
            event.set("tab"sv, tab_id);
            event.set("url"sv, String {});
            send_event(move(event));
        }
    };

    tab.view->on_favicon_change = [this, tab_id](Optional<Gfx::Bitmap const&> favicon) {
        auto* tab = tab_for_id(tab_id);
        if (!tab)
            return;

        JsonObject event;
        event.set("type"sv, "favicon"sv);
        event.set("tab"sv, tab_id);

        if (!favicon.has_value()) {
            event.set("path"sv, String {});
            send_event(move(event));
            return;
        }

        auto png = Gfx::PNGWriter::encode(*favicon);
        if (png.is_error())
            return;

        auto path = ByteString::formatted("{}/favicon-{}.png", m_frame_directory, tab_id);
        auto temp_path = ByteString::formatted("{}.tmp", path);
        auto file = Core::File::open(temp_path.view(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, 0600);
        if (file.is_error())
            return;
        auto& png_bytes = png.value();
        if (file.value()->write_until_depleted(png_bytes.bytes()).is_error())
            return;
        file.value()->close();
        if (Core::System::rename(temp_path.view(), path.view()).is_error())
            return;

        event.set("path"sv, path.view());
        send_event(move(event));
    };

    tab.view->on_request_alert = [this, tab_id](Utf16String const& message) {
        if (auto* tab = tab_for_id(tab_id)) {
            auto text = message.to_utf8();
            send_dialog(*tab, "alert"sv, text.bytes_as_string_view());
        }
    };

    tab.view->on_request_confirm = [this, tab_id](Utf16String const& message) {
        if (auto* tab = tab_for_id(tab_id)) {
            auto text = message.to_utf8();
            send_dialog(*tab, "confirm"sv, text.bytes_as_string_view());
        }
    };

    tab.view->on_request_prompt = [this, tab_id](Utf16String const& message, Utf16String const& default_text) {
        if (auto* tab = tab_for_id(tab_id)) {
            auto text = message.to_utf8();
            auto default_value = default_text.to_utf8();
            send_dialog(*tab, "prompt"sv, text.bytes_as_string_view(), default_value.bytes_as_string_view());
        }
    };

    tab.view->on_request_before_unload = [](String const&, Function<void(bool)> on_complete) {
        // A tab close is a deliberate user action in this frontend, so do not
        // make the user answer a second dialog. A real implementation should
        // route this through the chrome like the other dialogs.
        on_complete(true);
    };

    tab.view->on_new_web_view = [this, tab_id](Web::HTML::ActivateTab, Web::HTML::WebViewHints, WebView::CanonicalTraversable& traversable) -> String {
        auto* parent = tab_for_id(tab_id);
        if (!parent)
            return {};
        auto& child = create_child_tab(*parent, traversable);
        return child.view->handle();
    };

    tab.view->on_fullscreen_window = [this, tab_id] {
        if (auto* tab = tab_for_id(tab_id)) {
            tab->view->set_is_fullscreen(Web::ViewportIsFullscreen::Yes);
            JsonObject event;
            event.set("type"sv, "fullscreen"sv);
            event.set("tab"sv, tab_id);
            event.set("fullscreen"sv, true);
            send_event(move(event));
        }
    };

    tab.view->on_exit_fullscreen_window = [this, tab_id] {
        if (auto* tab = tab_for_id(tab_id)) {
            tab->view->set_is_fullscreen(Web::ViewportIsFullscreen::No);
            JsonObject event;
            event.set("type"sv, "fullscreen"sv);
            event.set("tab"sv, tab_id);
            event.set("fullscreen"sv, false);
            send_event(move(event));
        }
    };

    tab.view->on_close = [this, tab_id] {
        // The close callback runs inside the view's own dispatch; erase the tab
        // after the call stack unwinds.
        Core::deferred_invoke([this, tab_id] {
            destroy_tab(tab_id);
        });
    };

    tab.view->on_web_content_crashed = [this, tab_id](auto) {
        if (tab_for_id(tab_id))
            send_simple_event("tab_crashed"sv, tab_id);
    };

    auto wire_context_menu = [this, tab_id](WebView::Menu& menu, StringView kind) {
        auto* menu_pointer = &menu;
        auto kind_string = ByteString { kind };
        menu.on_activation = [this, tab_id, menu_pointer, kind_string](Gfx::IntPoint position) {
            if (auto* tab = tab_for_id(tab_id))
                send_context_menu(*tab, kind_string.view(), position, *menu_pointer);
        };
    };
    wire_context_menu(tab.view->page_context_menu(), "page"sv);
    wire_context_menu(tab.view->link_context_menu(), "link"sv);
    wire_context_menu(tab.view->selected_text_link_context_menu(), "link"sv);
    wire_context_menu(tab.view->image_context_menu(), "image"sv);
    wire_context_menu(tab.view->media_context_menu(), "media"sv);

    tab.view->on_input_method_state_change = [this, tab_id] {
        auto* tab = tab_for_id(tab_id);
        if (!tab)
            return;
        auto const& state = tab->view->input_method_state();
        JsonObject event;
        event.set("type"sv, "input_method_state"sv);
        event.set("tab"sv, tab_id);
        event.set("enabled"sv, state.is_enabled);
        if (state.caret_rect.has_value()) {
            event.set("x"sv, state.caret_rect->x().value());
            event.set("y"sv, state.caret_rect->y().value());
            event.set("width"sv, state.caret_rect->width().value());
            event.set("height"sv, state.caret_rect->height().value());
        }
        send_event(move(event));
    };

    tab.view->on_find_in_page = [this, tab_id](size_t current_match_index, Optional<size_t> const& total_match_count) {
        if (!tab_for_id(tab_id))
            return;

        JsonObject event;
        event.set("type"sv, "find_result"sv);
        event.set("tab"sv, tab_id);
        event.set("current"sv, static_cast<u64>(current_match_index));
        if (total_match_count.has_value())
            event.set("total"sv, static_cast<u64>(*total_match_count));
        send_event(move(event));
    };

    tab.view->on_request_permission = [this, tab_id](Utf16String const& name, URL::Origin const& origin, Function<void(bool)> on_decided) {
        auto* tab = tab_for_id(tab_id);
        if (!tab) {
            on_decided(false);
            return;
        }

        auto request_id = m_next_permission_id++;
        tab->pending_permissions.set(request_id, move(on_decided));
        send_permission_request(*tab, name, origin, request_id);
    };

    tab.view->on_request_file_picker = [this, tab_id](Web::HTML::FileFilter const& filters, Web::HTML::AllowMultipleFiles multiple) {
        if (auto* tab = tab_for_id(tab_id))
            send_file_picker_request(*tab, filters, multiple);
    };

    tab.view->on_request_select_dropdown = [this, tab_id](Gfx::IntPoint position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        if (auto* tab = tab_for_id(tab_id))
            send_select_dropdown(*tab, position, minimum_width, items);
    };
}

void EngineServer::destroy_tab(u64 tab_id)
{
    auto tab = m_tabs.take(tab_id);
    if (!tab.has_value())
        return;

    destroy_frame_buffer(**tab);
    auto favicon_path = ByteString::formatted("{}/favicon-{}.png", m_frame_directory, tab_id);
    (void)Core::System::unlink(favicon_path.view());

    send_simple_event("tab_closed"sv, tab_id);

    if (m_focused_tab_id == tab_id)
        m_focused_tab_id = 0;
    if (m_active_tab_id == tab_id && !m_tabs.is_empty())
        m_active_tab_id = m_tabs.begin()->key;
    for (auto& [window_id, active_tab_id] : m_window_active_tabs) {
        if (active_tab_id == tab_id) {
            active_tab_id = 0;
            for (auto& [candidate_id, candidate] : m_tabs) {
                if (candidate->window_id == window_id) {
                    active_tab_id = candidate_id;
                    break;
                }
            }
            break;
        }
    }
}

void EngineServer::handle_create_tab(JsonObject const& command)
{
    auto width = command.get_i64("width"sv).value_or(1024);
    auto height = command.get_i64("height"sv).value_or(768);
    auto is_private = command.get_bool("private"sv).value_or(false);
    auto device_pixel_ratio = clamp(static_cast<int>(command.get_i64("scale"sv).value_or(1)), 1, 4);

    auto request_id = command.get_u64("request"sv).value_or(0);
    auto window_id = command.get_u64("window"sv).value_or(0);
    auto session_tab = command.get_u64("session_tab"sv);
    auto session_window = command.get_u64("session_window"sv);

    auto& tab = create_tab({ max(static_cast<int>(width), 1), max(static_cast<int>(height), 1) }, is_private, request_id, 0, window_id, device_pixel_ratio);

    auto* store = WebView::Application::session_store(WebView::IsPrivate::No);

    if (session_window.has_value() && *session_window != 0)
        m_session_windows.set(window_id, static_cast<WebView::SessionWindowId>(*session_window));

    if (!is_private && store && !Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv)) {
        if (session_tab.has_value() && *session_tab != 0) {
            auto tab_id = static_cast<WebView::SessionTabId>(*session_tab);
            tab.session_tab_id = tab_id;
            tab.view->set_session_tab_id(tab_id);
        } else {
            auto initial_url = WebView::Application::settings().new_tab_page_url();
            if (auto url = command.get_string("url"sv); url.has_value() && !url->is_empty()) {
                if (auto parsed = URL::Parser::basic_parse(url->bytes_as_string_view()); parsed.has_value())
                    initial_url = parsed.release_value();
            }
            auto session_window_id = session_window_for(window_id, true);
            auto tab_id = store->tab_opened({
                .window_id = session_window_id,
                .initial_url = move(initial_url),
                .insertion_index = {},
                .is_active = WebView::SessionStore::IsActive::No,
            });
            if (!tab_id.is_error()) {
                tab.session_tab_id = tab_id.value();
                tab.view->set_session_tab_id(tab_id.value());
            }
        }
    }

    if (tab.session_tab_id.has_value() && store) {
        auto history = store->load_tab_history(*tab.session_tab_id);
        if (history.has_value() && !tab.view->restore_session_history_from_snapshot(history.release_value()).is_error())
            return;
    }

    if (auto url = command.get_string("url"sv); url.has_value() && !url->is_empty())
        tab.view->load_from_user_input(url->bytes_as_string_view());
    else
        tab.view->load(WebView::Application::settings().new_tab_page_url());
}

void EngineServer::handle_close_window(JsonObject const& command)
{
    auto session_window = command.get_u64("session_window"sv);
    if (!session_window.has_value() || *session_window == 0)
        return;

    auto window_id = static_cast<WebView::SessionWindowId>(*session_window);
    if (auto* store = WebView::Application::session_store(WebView::IsPrivate::No)) {
        auto active_tab = static_cast<i64>(command.get_u64("active"sv).value_or(0));
        (void)store->window_closing({
            .window_id = window_id,
            .active_tab_index = active_tab,
            .closed_at = UnixDateTime::now(),
        });
    }

    Vector<u64> windows_to_remove;
    for (auto const& [frontend_window_id, mapped_window_id] : m_session_windows) {
        if (mapped_window_id == window_id)
            windows_to_remove.append(frontend_window_id);
    }
    for (auto frontend_window_id : windows_to_remove)
        m_session_windows.remove(frontend_window_id);
}

void EngineServer::handle_window_geometry(JsonObject const& command)
{
    auto session_window = command.get_u64("session_window"sv);
    if (!session_window.has_value() || *session_window == 0)
        return;

    auto* store = WebView::Application::session_store(WebView::IsPrivate::No);
    if (!store)
        return;

    store->window_geometry_changed(
        static_cast<WebView::SessionWindowId>(*session_window),
        {
            .x = static_cast<i32>(command.get_i64("x"sv).value_or(0)),
            .y = static_cast<i32>(command.get_i64("y"sv).value_or(0)),
            .width = static_cast<i32>(command.get_i64("width"sv).value_or(0)),
            .height = static_cast<i32>(command.get_i64("height"sv).value_or(0)),
            .maximized = command.get_bool("maximized"sv).value_or(false),
        });
}

void EngineServer::handle_register_session_tab(JsonObject const& command)
{
    auto* store = WebView::Application::session_store(WebView::IsPrivate::No);
    auto session_window = command.get_u64("session_window"sv);
    auto request = command.get_u64("request"sv);
    auto window = command.get_u64("window"sv).value_or(0);
    auto url = command.get_string("url"sv);
    if (!store || !session_window.has_value() || !request.has_value() || !url.has_value() || url->is_empty())
        return;

    auto parsed = URL::Parser::basic_parse(url->bytes_as_string_view());
    if (!parsed.has_value())
        return;

    auto tab_id = store->tab_opened({
        .window_id = static_cast<WebView::SessionWindowId>(*session_window),
        .initial_url = parsed.release_value(),
        .insertion_index = {},
        .is_active = WebView::SessionStore::IsActive::No,
    });
    if (tab_id.is_error())
        return;

    JsonObject event;
    event.set("type"sv, "session_tab_registered"sv);
    event.set("request"sv, *request);
    event.set("window"sv, window);
    event.set("session_tab"sv, static_cast<u64>(tab_id.value()));
    send_event(move(event));
}

WebView::SessionWindowId EngineServer::session_window_for(u64 frontend_window_id, bool create)
{
    if (auto it = m_session_windows.find(frontend_window_id); it != m_session_windows.end())
        return it->value;

    if (!create)
        return 0;

    auto* store = WebView::Application::session_store(WebView::IsPrivate::No);
    if (!store)
        return 0;

    auto session_window_id = store->window_opened();
    if (session_window_id.is_error())
        return 0;

    m_session_windows.set(frontend_window_id, session_window_id.value());

    JsonObject event;
    event.set("type"sv, "session_window"sv);
    event.set("window"sv, frontend_window_id);
    event.set("session_window"sv, static_cast<u64>(session_window_id.value()));
    send_event(move(event));

    return session_window_id.value();
}

void EngineServer::handle_activate_tab(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto window_id = command.get_u64("window"sv).value_or(tab->window_id);
    if (window_id != 0) {
        tab->window_id = window_id;
        m_window_active_tabs.set(window_id, tab_id);
    }
    m_active_tab_id = tab_id;
    if (tab->session_tab_id.has_value()) {
        if (auto* store = WebView::Application::session_store(WebView::IsPrivate::No))
            store->active_tab_changed(*tab->session_tab_id);
    }

    for (auto& [id, other_tab] : m_tabs) {
        bool visible = false;
        if (other_tab->window_id != 0)
            visible = m_window_active_tabs.get(other_tab->window_id).value_or(0) == id;
        else
            visible = id == tab_id;
        other_tab->view->set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
    }
}

void EngineServer::handle_navigate(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto url = command.get_string("url"sv);
    auto user_input = command.get_bool("user_input"sv).value_or(true);

    // The UI process owns the session-history mirror. Starting a new navigation

    // while the previous one is still committing can hand WebContent a target
    // entry from the wrong document state. Queue the latest requested
    // navigation and start it when the current load finishes.
    if (tab->view->is_loading() || tab->pending_navigation.has_value()) {
        tab->pending_navigation = url.has_value() && !url->is_empty()
            ? *url
            : WebView::Application::settings().new_tab_page_url().to_string();
        tab->pending_navigation_user_input = user_input;
        return;
    }

    perform_navigation(*tab, url.has_value() ? *url : String {}, user_input);
}

void EngineServer::perform_navigation(Tab& tab, String const& url, bool user_input)
{
    if (url.is_empty()) {
        tab.view->load(WebView::Application::settings().new_tab_page_url());
        return;
    }

    if (user_input)
        tab.view->load_from_user_input(url.bytes_as_string_view());
    else if (auto parsed = URL::Parser::basic_parse(url.bytes_as_string_view()); parsed.has_value())
        tab.view->load(*parsed);
}

void EngineServer::flush_pending_navigation(Tab& tab)
{
    if (!tab.pending_navigation.has_value())
        return;

    auto url = tab.pending_navigation.release_value();
    auto user_input = tab.pending_navigation_user_input;
    perform_navigation(tab, url, user_input);
}

void EngineServer::handle_autocomplete(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->autocomplete)
        return;

    auto request_id = command.get_u64("request"sv);
    if (!request_id.has_value())
        return;

    auto query = command.get_string("query"sv);
    if (!query.has_value() || query->is_empty()) {
        tab->autocomplete->cancel_pending_query();
        send_autocomplete_results(tab_id, *request_id, {}, WebView::AutocompleteResultKind::Final);
        return;
    }

    tab->autocomplete->query_autocomplete_engine(*request_id, *query);
}

void EngineServer::handle_resize(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto width = max(static_cast<int>(command.get_i64("width"sv).value_or(1)), 1);
    auto height = max(static_cast<int>(command.get_i64("height"sv).value_or(1)), 1);
    auto size = Gfx::IntSize { width, height };
    if (tab->viewport_size == size)
        return;

    tab->viewport_size = size;
    tab->view->reset_viewport_size(size.to_type<Web::DevicePixels>());
}

void EngineServer::handle_focus(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    tab->focused = command.get_bool("focused"sv).value_or(false);
    if (tab->focused)
        m_focused_tab_id = tab->id;
    else if (m_focused_tab_id == tab->id)
        m_focused_tab_id = 0;
    tab->view->set_has_system_focus(tab->focused);
}

static Web::UIEvents::KeyModifier modifiers_from_command(JsonObject const& command)
{
    auto modifiers = Web::UIEvents::KeyModifier::Mod_None;
    if (command.get_bool("ctrl"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (command.get_bool("shift"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    if (command.get_bool("alt"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (command.get_bool("super"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Super;
    return modifiers;
}

void EngineServer::handle_mouse(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("event"sv);
    if (!kind.has_value())
        return;

    auto scale = max(tab->device_pixel_ratio, 1);
    Web::MouseEvent event;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)) * scale, static_cast<int>(command.get_i64("y"sv).value_or(0)) * scale };
    event.screen_position = event.position;
    event.modifiers = modifiers_from_command(command);

    auto button = command.get_string("button"sv);
    auto button_name = button.has_value() ? button->bytes_as_string_view() : "none"sv;
    if (button_name == "left"sv)
        event.button = Web::UIEvents::MouseButton::Primary;
    else if (button_name == "right"sv)
        event.button = Web::UIEvents::MouseButton::Secondary;
    else if (button_name == "middle"sv)
        event.button = Web::UIEvents::MouseButton::Middle;
    else if (button_name == "back"sv)
        event.button = Web::UIEvents::MouseButton::Backward;
    else if (button_name == "forward"sv)
        event.button = Web::UIEvents::MouseButton::Forward;
    else
        event.button = Web::UIEvents::MouseButton::None;

    event.buttons = static_cast<Web::UIEvents::MouseButton>(command.get_u64("buttons"sv).value_or(0));
    event.click_count = static_cast<int>(command.get_i64("click_count"sv).value_or(0));

    if (*kind == "down"sv)
        event.type = Web::MouseEvent::Type::MouseDown;
    else if (*kind == "up"sv)
        event.type = Web::MouseEvent::Type::MouseUp;
    else if (*kind == "move"sv)
        event.type = Web::MouseEvent::Type::MouseMove;
    else if (*kind == "leave"sv)
        event.type = Web::MouseEvent::Type::MouseLeave;
    else
        return;

    tab->view->enqueue_input_event(move(event));
}

void EngineServer::handle_wheel(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto scale = max(tab->device_pixel_ratio, 1);
    Web::MouseEvent event;
    event.type = Web::MouseEvent::Type::MouseWheel;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)) * scale, static_cast<int>(command.get_i64("y"sv).value_or(0)) * scale };
    event.screen_position = event.position;
    event.modifiers = modifiers_from_command(command);
    event.button = Web::UIEvents::MouseButton::None;
    event.buttons = Web::UIEvents::MouseButton::None;

    event.wheel_delta_x = command.get_double_with_precision_loss("delta_x"sv).value_or(0.0);
    event.wheel_delta_y = command.get_double_with_precision_loss("delta_y"sv).value_or(0.0);
    event.wheel_delta_precision = Web::WheelDeltaPrecision::Discrete;
    event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, event.wheel_delta_precision);
    event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, event.wheel_delta_precision);

    if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
        warnln("LadybirdEngine: tab {} wheel delta {}x{}", tab_id, event.wheel_delta_x, event.wheel_delta_y);
    ++tab->wheel_events_since_frame;
    tab->view->enqueue_input_event(move(event));
    note_user_input(*tab);
}

void EngineServer::handle_key(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("event"sv);
    if (!kind.has_value())
        return;

    Web::KeyEvent event;
    if (*kind == "down"sv)
        event.type = Web::KeyEvent::Type::KeyDown;
    else if (*kind == "up"sv)
        event.type = Web::KeyEvent::Type::KeyUp;
    else
        return;

    auto name = command.get_string("key"sv);
    auto key_string = Utf16String::from_utf8_with_replacement_character(name.has_value() ? name->bytes_as_string_view() : ""sv);
    event.key = Web::UIEvents::key_code_from_string(key_string.utf16_view());
    event.modifiers = modifiers_from_command(command);
    event.code_point = static_cast<u32>(command.get_u64("codepoint"sv).value_or(0));
    event.repeat = command.get_bool("repeat"sv).value_or(false);

    // A printable key press with no command modifier is text for the page. The
    // platform's input method may later drive composition, but this covers
    // ordinary typing in the LibGUI frontend.
    auto has_command_modifier = (event.modifiers & (Web::UIEvents::KeyModifier::Mod_Ctrl | Web::UIEvents::KeyModifier::Mod_Alt | Web::UIEvents::KeyModifier::Mod_Super)) != Web::UIEvents::KeyModifier::Mod_None;
    event.should_insert_text = event.type == Web::KeyEvent::Type::KeyDown && event.code_point != 0 && !has_command_modifier;
    auto should_watch_for_frame = event.type == Web::KeyEvent::Type::KeyDown && !event.repeat;

    tab->view->enqueue_input_event(move(event));
    if (should_watch_for_frame)
        note_user_input(*tab);
}

void EngineServer::handle_dialog_response(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("kind"sv);
    auto kind_name = kind.has_value() ? kind->bytes_as_string_view() : ""sv;
    auto accepted = command.get_bool("accept"sv).value_or(true);

    if (kind_name == "alert"sv) {
        tab->view->alert_closed();
    } else if (kind_name == "confirm"sv) {
        tab->view->confirm_closed(accepted);
    } else if (kind_name == "prompt"sv) {
        if (accepted) {
            auto text = command.get_string("text"sv);
            tab->view->prompt_closed(Utf16String::from_utf8_with_replacement_character(text.has_value() ? text->bytes_as_string_view() : ""sv));
        } else {
            tab->view->prompt_closed({});
        }
    }
}

void EngineServer::handle_close_tab(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || tab->closing)
        return;

    if (tab->view->is_private() == WebView::IsPrivate::No && tab->session_tab_id.has_value()) {
        if (auto* store = WebView::Application::session_store(WebView::IsPrivate::No)) {
            (void)store->tab_closed({
                .tab_id = *tab->session_tab_id,
                .closed_at = UnixDateTime::now(),
                .keep_for_reopening = WebView::SessionStore::KeepForReopening::Yes,
            });
        }
    }

    tab->closing = true;
    tab->view->request_close();
}

void EngineServer::handle_zoom(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto direction = command.get_string("direction"sv);
    auto direction_name = direction.has_value() ? direction->bytes_as_string_view() : "reset"sv;
    if (direction_name == "in"sv)
        tab->view->zoom_in();
    else if (direction_name == "out"sv)
        tab->view->zoom_out();
    else
        tab->view->reset_zoom();
}

void EngineServer::handle_find(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto query = command.get_string("query"sv);
    if (!query.has_value() || query->is_empty()) {
        tab->view->find_in_page_end();
        return;
    }

    auto case_sensitivity = command.get_bool("match_case"sv).value_or(false)
        ? CaseSensitivity::CaseSensitive
        : CaseSensitivity::CaseInsensitive;
    tab->view->find_in_page(Utf16String::from_utf8_with_replacement_character(query->bytes_as_string_view()), case_sensitivity, true);
}

void EngineServer::destroy_frame_buffer(Tab& tab)
{
    if (tab.frame_mapping) {
        (void)::munmap(tab.frame_mapping, tab.frame_mapping_size);
        tab.frame_mapping = nullptr;
        tab.frame_mapping_size = 0;
    }
    if (tab.frame_fd >= 0) {
        ::close(tab.frame_fd);
        tab.frame_fd = -1;
    }
    if (!tab.frame_buffer_name.is_empty()) {
        ::shm_unlink(tab.frame_buffer_name.characters());
        tab.frame_buffer_name = {};
    }
    tab.frame_slot_size = 0;
    tab.frame_buffer_dimensions = {};
    tab.frame_write_slot = 0;
    reset_frame_tracking(tab);
}

bool EngineServer::ensure_frame_buffer(Tab& tab, Gfx::IntSize dimensions)
{
    auto width = dimensions.width();
    auto height = dimensions.height();
    if (width <= 0 || height <= 0)
        return false;

    auto slot_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (tab.frame_mapping && tab.frame_slot_size >= slot_size && tab.frame_buffer_dimensions == dimensions)
        return true;

    // Recreate on a size change even when the old slots are large enough: the
    // in-flight slots still contain the old dimensions, and their acks refer to
    // a layout the frontend can no longer use safely.
    destroy_frame_buffer(tab);

    tab.frame_slot_size = slot_size;
    tab.frame_buffer_dimensions = dimensions;

    auto name = ByteString::formatted("/ladybird-sde-{}-{}-{}", ::getpid(), tab.id, m_next_frame_buffer_id++);
    int fd = ::shm_open(name.characters(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        warnln("LadybirdEngine: could not create frame buffer '{}': {}", name, strerror(errno));
        tab.frame_slot_size = 0;
        return false;
    }

    auto bytes = tab.frame_slot_size * static_cast<size_t>(tab.frame_slots);
    if (::ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
        warnln("LadybirdEngine: could not resize frame buffer: {}", strerror(errno));
        ::close(fd);
        ::shm_unlink(name.characters());
        tab.frame_slot_size = 0;
        return false;
    }

    auto* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        warnln("LadybirdEngine: could not map frame buffer: {}", strerror(errno));
        ::close(fd);
        ::shm_unlink(name.characters());
        tab.frame_slot_size = 0;
        return false;
    }

    tab.frame_fd = fd;
    tab.frame_mapping = mapping;
    tab.frame_mapping_size = bytes;
    tab.frame_buffer_name = name;
    tab.frame_write_slot = 0;

    JsonObject event;
    event.set("type"sv, "frame_buffer"sv);
    event.set("tab"sv, tab.id);
    event.set("name"sv, tab.frame_buffer_name.view());
    event.set("slots"sv, tab.frame_slots);
    event.set("slot_size"sv, static_cast<u64>(tab.frame_slot_size));
    event.set("width"sv, width);
    event.set("height"sv, height);
    event.set("scale"sv, tab.device_pixel_ratio);
    send_event(move(event));
    return true;
}

void EngineServer::handle_find_next(u64 tab_id, bool forward)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    if (forward)
        tab->view->find_in_page_next_match();
    else
        tab->view->find_in_page_previous_match();
}

void EngineServer::handle_toggle_bookmark(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    WebView::Application::the().toggle_bookmark_for_view(*tab->view);
    send_url(*tab);
    send_bookmarks();
}

static bool clipboard_mime_is_textual(String const& name)
{
    return name.starts_with_bytes("text/"sv)
        || name == "application/json"sv
        || name == "application/xml"sv
        || name == "application/xhtml+xml"sv
        || name == "image/svg+xml"sv;
}

static JsonObject clipboard_representation_to_json(Web::Clipboard::SystemClipboardRepresentation const& representation)
{
    JsonObject object;
    object.set("name"sv, representation.name);

    auto const* bytes = representation.data.get_pointer<ByteString>();
    if (!bytes)
        return object;

    if (clipboard_mime_is_textual(representation.name)) {
        if (auto text = String::from_utf8(bytes->view()); !text.is_error()) {
            object.set("data"sv, text.value());
            return object;
        }
    }

    auto encoded = encode_base64(bytes->bytes());
    if (encoded.is_error())
        return object;
    object.set("data"sv, encoded.value());
    object.set("encoding"sv, "base64"sv);
    return object;
}

static JsonArray clipboard_item_to_json(Web::Clipboard::SystemClipboardItem const& item)
{
    JsonArray items;
    for (auto const& representation : item.system_clipboard_representations) {
        auto object = clipboard_representation_to_json(representation);
        if (object.has("data"sv))
            items.must_append(move(object));
    }
    return items;
}

static Optional<Web::Clipboard::SystemClipboardItem> clipboard_item_from_json(JsonArray const& items)
{
    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;

    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        auto name = object.get_string("name"sv);
        auto data = object.get_string("data"sv);
        if (!name.has_value() || !data.has_value())
            continue;

        ByteString bytes;
        auto encoding = object.get_string("encoding"sv);
        if (encoding.has_value() && *encoding == "base64"sv) {
            auto decoded = decode_base64(*data);
            if (decoded.is_error())
                continue;
            bytes = ByteString::copy(decoded.value());
        } else {
            bytes = data->to_byte_string();
        }

        representations.append({ move(*name), move(bytes) });
    }

    if (representations.is_empty())
        return {};
    return Web::Clipboard::SystemClipboardItem { move(representations) };
}

void EngineServer::handle_history_recent(JsonObject const& command)
{
    auto limit = static_cast<size_t>(min<u64>(command.get_u64("limit"sv).value_or(10), 50));

    JsonArray items;
    if (auto* store = WebView::Application::default_session().history_store.ptr()) {
        for (auto const& entry : store->list_entries({}, 0, limit)) {
            JsonObject item;
            item.set("url"sv, entry.url);
            item.set("title"sv, entry.title.value_or(entry.url));
            items.must_append(move(item));
        }
    }

    JsonObject event;
    event.set("type"sv, "history_recent"sv);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::handle_history_clear()
{
    if (auto* store = WebView::Application::default_session().history_store.ptr())
        store->remove_entries_accessed_since(UnixDateTime::earliest());

    JsonObject event;
    event.set("type"sv, "history_recent"sv);
    event.set("items"sv, JsonArray {});
    send_event(move(event));
}

void EngineServer::handle_add_bookmark(JsonObject const& command)
{
    auto url_text = command.get_string("url"sv);
    if (!url_text.has_value())
        return;
    auto url = URL::Parser::basic_parse(url_text->bytes_as_string_view());
    if (!url.has_value())
        return;

    Optional<String> title;
    if (auto text = command.get_string("title"sv); text.has_value())
        title = *text;

    Optional<String> parent;
    if (auto text = command.get_string("parent"sv); text.has_value() && !text->is_empty())
        parent = *text;

    if (parent.has_value())
        WebView::Application::bookmark_store().add_bookmark(url.release_value(), move(title), {}, *parent);
    else
        WebView::Application::bookmark_store().add_bookmark(url.release_value(), move(title), {});
    send_bookmarks();
    if (auto tab_id = command.get_u64("tab"sv); tab_id.has_value()) {
        if (auto* tab = tab_for_id(*tab_id))
            send_url(*tab);
    }
}

void EngineServer::handle_edit_bookmark(JsonObject const& command)
{
    auto id = command.get_string("id"sv);
    auto url_text = command.get_string("url"sv);
    if (!id.has_value() || !url_text.has_value())
        return;
    auto url = URL::Parser::basic_parse(url_text->bytes_as_string_view());
    if (!url.has_value())
        return;

    Optional<String> title;
    if (auto text = command.get_string("title"sv); text.has_value())
        title = *text;
    WebView::Application::bookmark_store().edit_bookmark(*id, url.release_value(), move(title));
    send_bookmarks();
    if (auto tab_id = command.get_u64("tab"sv); tab_id.has_value()) {
        if (auto* tab = tab_for_id(*tab_id))
            send_url(*tab);
    }
}

void EngineServer::handle_remove_bookmark(JsonObject const& command)
{
    auto id = command.get_string("id"sv);
    if (!id.has_value())
        return;

    WebView::Application::bookmark_store().remove_item(*id);
    send_bookmarks();
    if (auto tab_id = command.get_u64("tab"sv); tab_id.has_value()) {
        if (auto* tab = tab_for_id(*tab_id))
            send_url(*tab);
    }
}

void EngineServer::handle_history_search(JsonObject const& command)
{
    String query;
    if (auto text = command.get_string("query"sv); text.has_value())
        query = *text;
    auto limit = static_cast<size_t>(min<u64>(command.get_u64("limit"sv).value_or(50), 200));

    JsonArray items;
    if (auto* store = WebView::Application::default_session().history_store.ptr()) {
        for (auto const& entry : store->list_entries(query.bytes_as_string_view(), 0, limit)) {
            JsonObject item;
            item.set("url"sv, entry.url);
            item.set("title"sv, entry.title.value_or(entry.url));
            items.must_append(move(item));
        }
    }

    JsonObject event;
    event.set("type"sv, "history_search_results"sv);
    event.set("query"sv, query.bytes_as_string_view());
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::handle_add_folder(JsonObject const& command)
{
    Optional<String> title;
    if (auto text = command.get_string("title"sv); text.has_value())
        title = *text;

    auto parent_text = command.get_string("parent"sv);
    if (parent_text.has_value() && !parent_text->is_empty()) {
        String parent = *parent_text;
        WebView::Application::bookmark_store().add_folder(move(title), parent);
    } else {
        WebView::Application::bookmark_store().add_folder(move(title), {});
    }
    send_bookmarks();
}

void EngineServer::handle_edit_folder(JsonObject const& command)
{
    auto id = command.get_string("id"sv);
    if (!id.has_value())
        return;

    Optional<String> title;
    if (auto text = command.get_string("title"sv); text.has_value())
        title = *text;

    WebView::Application::bookmark_store().edit_folder(*id, move(title));
    send_bookmarks();
}

void EngineServer::handle_move_bookmark(JsonObject const& command)
{
    auto id = command.get_string("id"sv);
    if (!id.has_value())
        return;

    auto index = static_cast<size_t>(command.get_u64("index"sv).value_or(0));
    auto parent_text = command.get_string("parent"sv);
    if (parent_text.has_value() && !parent_text->is_empty()) {
        String parent = *parent_text;
        WebView::Application::bookmark_store().move_item(*id, parent, index);
    } else {
        WebView::Application::bookmark_store().move_item(*id, {}, index);
    }
    send_bookmarks();
}

void EngineServer::handle_pin_tab(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->session_tab_id.has_value())
        return;

    if (auto* store = WebView::Application::session_store(WebView::IsPrivate::No))
        (void)store->tab_pinned_changed(*tab->session_tab_id, command.get_bool("pinned"sv).value_or(false));
}

void EngineServer::handle_toggle_mute(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    tab->view->toggle_page_mute_state();
    send_audio_state(*tab);
}

void EngineServer::handle_ime_marked_text(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;
    String text;
    if (auto value = command.get_string("text"sv); value.has_value())
        text = *value;
    tab->view->set_marked_text_from_input_method(Utf16String::from_utf8_with_replacement_character(text));
}

void EngineServer::handle_ime_commit_text(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;
    String text;
    if (auto value = command.get_string("text"sv); value.has_value())
        text = *value;
    auto replacement_start = static_cast<i32>(command.get_i64("replacement_start"sv).value_or(0));
    auto replacement_length = static_cast<i32>(command.get_i64("replacement_length"sv).value_or(0));
    tab->view->commit_text_from_input_method(Utf16String::from_utf8_with_replacement_character(text), replacement_start, replacement_length);
}

void EngineServer::handle_ime_unmark_text(u64 tab_id)
{
    if (auto* tab = tab_for_id(tab_id))
        tab->view->unmark_text_from_input_method();
}

void EngineServer::handle_certificate_info(u64 tab_id, JsonObject const&)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto url = tab->view->url();
    if (url.scheme() != "https"sv) {
        send_certificate_error(tab_id, "Certificate details are only available for secure pages."_string);
        return;
    }

    // A fresh HEAD request runs through the same RequestServer and TLS stack as the page, so the certificate we report
    // is the one this profile would actually negotiate. Any previous lookup for the tab is cancelled first.
    if (auto previous = m_certificate_requests.take(tab_id); previous.has_value())
        (*previous)->stop();

    auto client = WebView::Application::request_server_client();
    if (client.is_error()) {
        send_certificate_error(tab_id, "Could not connect to RequestServer."_string);
        return;
    }

    auto request = client.value()->start_request(
        "HEAD"sv,
        url,
        {},
        {},
        HTTP::CacheMode::NoStore,
        HTTP::Cookie::IncludeCredentials::No,
        Requests::RequestClient::TransferLease::No,
        0u,
        Requests::RequestClient::CacheMissNotification::No,
        0u,
        {},
        true);
    if (!request) {
        send_certificate_error(tab_id, "Could not start the certificate request."_string);
        return;
    }

    WeakPtr<Requests::Request> weak_request = request->make_weak_ptr();
    m_certificate_requests.set(tab_id, *request);

    request->set_unbuffered_request_callbacks(
        [this, tab_id, weak_request](auto&&, auto&&, auto&&, auto&&, auto&&, auto&&) {
            auto request = weak_request.strong_ref();
            if (!request)
                return;

            // A reused TLS connection may only expose the certificate after the transfer completes; the finish
            // callback below retries in that case.
            auto info = request->certificate_info();
            if (!info.has_value())
                return;

            send_certificate_info(tab_id, *info);
            m_certificate_requests.remove(tab_id);
            Core::deferred_invoke([request = request.release_nonnull()]() mutable {
                request->stop();
            });
        },
        [](Requests::ResponseData) {},
        [](Core::ImmutableBytes) {},
        [this, tab_id, weak_request](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error) {
            if (!m_certificate_requests.contains(tab_id))
                return;
            auto request = weak_request.strong_ref();

            if (request) {
                if (auto info = request->certificate_info(); info.has_value()) {
                    send_certificate_info(tab_id, *info);
                    m_certificate_requests.remove(tab_id);
                    return;
                }
            }

            if (network_error.has_value())
                send_certificate_error(tab_id, String::formatted("Could not reach the site ({}).", Requests::network_error_to_string(*network_error)).release_value());
            else
                send_certificate_error(tab_id, "The server did not present a certificate."_string);
            m_certificate_requests.remove(tab_id);
        });
}

void EngineServer::send_certificate_info(u64 tab_id, Requests::CertificateInfo const& info)
{
    JsonObject event;
    event.set("type"sv, "certificate_info"sv);
    event.set("tab"sv, tab_id);
    event.set("ok"sv, true);
    event.set("subject"sv, info.subject);
    event.set("issuer"sv, info.issuer);
    event.set("valid_from"sv, info.valid_from);
    event.set("valid_until"sv, info.valid_until);
    event.set("serial_number"sv, info.serial_number);
    event.set("sha256_fingerprint"sv, info.sha256_fingerprint);
    event.set("protocol"sv, info.protocol);
    event.set("cipher"sv, info.cipher);
    send_event(move(event));
}

void EngineServer::send_certificate_error(u64 tab_id, String const& error)
{
    JsonObject event;
    event.set("type"sv, "certificate_info"sv);
    event.set("tab"sv, tab_id);
    event.set("ok"sv, false);
    event.set("error"sv, error);
    send_event(move(event));
}

void EngineServer::send_audio_state(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "audio"sv);
    event.set("tab"sv, tab.id);
    event.set("playing"sv, tab.view->audio_play_state() == Web::HTML::AudioPlayState::Playing);
    event.set("muted"sv, tab.view->page_mute_state() == Web::HTML::MuteState::Muted);
    send_event(move(event));
}

void EngineServer::handle_clipboard_copy(u64 tab_id, bool cut)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto promise = cut ? tab->view->cut_selected_text() : tab->view->selected_text();
    promise->when_resolved([this, tab_id](ByteString& text) {
        if (text.is_empty())
            return;

        JsonObject event;
        event.set("type"sv, "clipboard"sv);
        event.set("tab"sv, tab_id);
        event.set("text"sv, text.view());
        send_event(move(event));
    });
}

void EngineServer::handle_paste(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    if (auto items = command.get_array("items"sv); items.has_value() && !items->is_empty()) {
        if (auto item = clipboard_item_from_json(*items); item.has_value())
            EngineApplication::the().cache_clipboard_item_from_frontend(item.release_value());
    } else if (auto text = command.get_string("text"sv); text.has_value()) {
        EngineApplication::the().cache_clipboard_item_from_frontend({ { { "text/plain"_string, text->to_byte_string() } } });
    } else {
        return;
    }

    tab->view->paste_from_clipboard();
}

void EngineServer::handle_clipboard_changed(JsonObject const& command)
{
    auto items = command.get_array("items"sv);
    if (!items.has_value())
        return;

    // An empty list means the desktop clipboard was cleared; forget the cached item so page-initiated reads do not
    // return something the user can no longer paste.
    if (auto item = clipboard_item_from_json(*items); item.has_value())
        EngineApplication::the().cache_clipboard_item_from_frontend(item.release_value());
    else
        EngineApplication::the().cache_clipboard_item_from_frontend({});
}

void EngineServer::handle_toggle_devtools()
{
    if (auto result = WebView::Application::the().toggle_devtools_enabled(); result.is_error()) {
        JsonObject event;
        event.set("type"sv, "devtools_error"sv);
        event.set("message"sv, MUST(String::formatted("Unable to start DevTools: {}", result.error())));
        send_event(move(event));
    }
}

void EngineServer::handle_open_devtools_client()
{
    if (auto result = WebView::Application::the().launch_devtools_client(); result.is_error()) {
        warnln("LadybirdEngine: unable to open the DevTools client: {}", result.error());
        JsonObject event;
        event.set("type"sv, "devtools_error"sv);
        event.set("message"sv, MUST(String::formatted("Unable to open the DevTools client: {}", result.error())));
        send_event(move(event));
    }
}

void EngineServer::handle_select_all(u64 tab_id)
{
    if (auto* tab = tab_for_id(tab_id))
        tab->view->select_all();
}

void EngineServer::handle_undo_redo(u64 tab_id, bool redo)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;
    if (redo)
        tab->view->redo();
    else
        tab->view->undo();
}

void EngineServer::reset_frame_tracking(Tab& tab)
{
    tab.frame_slot_states.clear();
    tab.frame_slot_states.ensure_capacity(tab.frame_slots);
    for (int i = 0; i < tab.frame_slots; ++i)
        tab.frame_slot_states.append({});
    tab.frame_damage_history.clear();
    tab.frame_content_size = {};
    tab.frame_write_slot = 0;
}

void EngineServer::on_view_painted(Tab& tab)
{
    if (tab.view->zoom_level() != tab.last_reported_zoom) {
        tab.last_reported_zoom = tab.view->zoom_level();
        send_zoom_changed(tab);
    }

    ++tab.presented_sequence;
    tab.frame_damage_history.append({ tab.presented_sequence, tab.view->last_presented_damage_rect() });
    if (tab.frame_damage_history.size() > 512)
        tab.frame_damage_history.remove(0, tab.frame_damage_history.size() - 512);
    tab.frame_dirty = true;
    try_send_frame(tab);
}

Gfx::IntRect EngineServer::damage_since(Tab& tab, u64 content_sequence, u64 latest_sequence, Gfx::IntSize size, bool& needs_full_copy)
{
    needs_full_copy = false;
    auto frame_rect = Gfx::IntRect { 0, 0, size.width(), size.height() };
    if (content_sequence >= latest_sequence)
        return {};
    if (content_sequence == 0) {
        needs_full_copy = true;
        return frame_rect;
    }

    bool found_damage = false;
    bool found_first = false;
    u64 first_sequence = 0;
    Gfx::IntRect union_rect;
    for (auto const& damage : tab.frame_damage_history) {
        if (damage.sequence <= content_sequence)
            continue;
        if (damage.sequence > latest_sequence)
            break;
        if (!found_first) {
            first_sequence = damage.sequence;
            found_first = true;
        }
        found_damage = true;
        auto rect = damage.rect.intersected(frame_rect);
        if (rect.is_empty())
            continue;
        union_rect = union_rect.is_empty() ? rect : union_rect.united(rect);
    }

    if (!found_damage || first_sequence > content_sequence + 1) {
        // The slot is older than the damage history we retained, so we cannot
        // know every changed pixel. Fall back to a full copy.
        needs_full_copy = true;
        return frame_rect;
    }

    if (union_rect.is_empty())
        return union_rect;

    auto frame_area = static_cast<u64>(size.width()) * static_cast<u64>(size.height());
    auto union_area = static_cast<u64>(union_rect.width()) * static_cast<u64>(union_rect.height());
    if (frame_area != 0 && union_area * 5 > frame_area * 3) {
        needs_full_copy = true;
        return frame_rect;
    }
    return union_rect;
}

void EngineServer::try_send_frame(Tab& tab)
{
    if (!tab.frame_dirty)
        return;

    auto bitmap = tab.view->displayed_bitmap();
    if (!bitmap)
        return;

    auto size = bitmap->size();
    if (size.is_empty())
        return;

    if (tab.last_reported_frame_size != size) {
        if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
            warnln("LadybirdEngine: tab {} viewport {}x{} frame {}x{} zoom {}", tab.id, tab.viewport_size.width(), tab.viewport_size.height(), size.width(), size.height(), tab.view->zoom_level());
        tab.last_reported_frame_size = size;
    }

    // A painted frame can briefly be larger than the requested viewport during a
    // resize. Size the ring for the frame, but do not feed the frame size back
    // into the requested viewport: doing so makes the host oscillate between the
    // old and new size and the frontend scales frames up and down.
    if (!ensure_frame_buffer(tab, size))
        return;

    if (tab.frame_content_size != size) {
        reset_frame_tracking(tab);
        tab.frame_content_size = size;
    }
    if (tab.frame_slot_states.size() != static_cast<size_t>(tab.frame_slots))
        reset_frame_tracking(tab);

    int slot = -1;
    u64 best_content_sequence = 0;
    for (int i = 0; i < tab.frame_slots; ++i) {
        auto const& state = tab.frame_slot_states[i];
        if (state.in_flight)
            continue;
        if (slot < 0 || state.content_sequence > best_content_sequence) {
            slot = i;
            best_content_sequence = state.content_sequence;
        }
    }
    if (slot < 0) {
        // All slots are still owned by the frontend. Keep frame_dirty set; the
        // next ack will publish the latest frame and intermediate frames are
        // coalesced instead of overwriting pixels in use.
        return;
    }

    auto& slot_state = tab.frame_slot_states[slot];
    bool full_copy = slot_state.content_sequence == 0;
    bool needs_full_copy = false;
    auto damage = Gfx::IntRect { 0, 0, size.width(), size.height() };
    if (!full_copy) {
        damage = damage_since(tab, slot_state.content_sequence, tab.presented_sequence, size, needs_full_copy);
        full_copy = needs_full_copy;
    }

    if (!full_copy && damage.is_empty()) {
        slot_state.content_sequence = tab.presented_sequence;
        tab.frame_dirty = false;
        return;
    }

    auto* destination = static_cast<u8*>(tab.frame_mapping) + (static_cast<size_t>(slot) * tab.frame_slot_size);
    auto stride = static_cast<size_t>(size.width()) * 4;

    if (full_copy) {
        damage = { 0, 0, size.width(), size.height() };
        for (int y = 0; y < size.height(); ++y)
            memcpy(destination + (static_cast<size_t>(y) * stride), bitmap->scanline_u8(y), stride);
    } else {
        for (int y = damage.y(); y < damage.bottom(); ++y) {
            memcpy(destination + (static_cast<size_t>(y) * stride) + static_cast<size_t>(damage.x()) * 4,
                bitmap->scanline_u8(y) + static_cast<size_t>(damage.x()) * 4,
                static_cast<size_t>(damage.width()) * 4);
        }
    }

    slot_state.content_sequence = tab.presented_sequence;
    slot_state.in_flight_sequence = tab.presented_sequence;
    slot_state.in_flight = true;
    tab.frame_write_slot = (slot + 1) % tab.frame_slots;
    tab.frame_dirty = false;

    if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
        warnln("LadybirdEngine: tab {} frame {} slot {} full={} damage {}x{}+{},{}", tab.id, tab.presented_sequence, slot, full_copy, damage.width(), damage.height(), damage.x(), damage.y());

    JsonObject event;
    event.set("type"sv, "frame"sv);
    event.set("tab"sv, tab.id);
    event.set("slot"sv, slot);
    event.set("width"sv, size.width());
    event.set("height"sv, size.height());
    event.set("stride"sv, static_cast<u64>(stride));
    event.set("scale"sv, tab.device_pixel_ratio);
    event.set("sequence"sv, tab.presented_sequence);
    event.set("full"sv, full_copy);
    event.set("damage_x"sv, damage.x());
    event.set("damage_y"sv, damage.y());
    event.set("damage_width"sv, damage.width());
    event.set("damage_height"sv, damage.height());
    send_event(move(event));
    note_frame_presented(tab, tab.presented_sequence);
}

void EngineServer::note_user_input(Tab& tab)
{
    ++tab.input_events_since_frame;
    if (tab.waiting_for_frame)
        return;
    if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
        warnln("LadybirdEngine: tab {} waiting for frame after input", tab.id);
    tab.waiting_for_frame = true;
    tab.input_wait_started = MonotonicTime::now();
    tab.last_cpu_sample_time = tab.input_wait_started;
    tab.last_cpu_ticks = web_content_cpu_ticks(tab.view->web_content_pid()).value_or(0);
}

void EngineServer::note_frame_presented(Tab& tab, u64 sequence)
{
    (void)sequence;
    tab.has_presented_frame = true;
    tab.input_events_since_frame = 0;
    tab.wheel_events_since_frame = 0;

    if (tab.unresponsive_reported) {
        // A frame that was already queued when the renderer spun can arrive
        // after the watchdog fires. Require two subsequent frames before
        // declaring the page responsive again; a genuinely recovered page will
        // paint again, while a single stale frame will not clear the banner.
        if (++tab.responsive_frame_count < 2)
            return;
        tab.unresponsive_reported = false;
        tab.responsive_frame_count = 0;
        set_unresponsive(tab, false);
    }

    tab.waiting_for_frame = false;
}

Optional<u64> EngineServer::web_content_cpu_ticks(pid_t pid)
{
    if (pid <= 0)
        return {};
    auto file = Core::File::open(ByteString::formatted("/proc/{}/stat", pid), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    auto buffer = contents.release_value();
    StringView text { reinterpret_cast<char const*>(buffer.data()), buffer.size() };
    Optional<size_t> close_paren;
    for (size_t i = text.length(); i > 0; --i) {
        if (text[i - 1] == ')') {
            close_paren = i - 1;
            break;
        }
    }
    if (!close_paren.has_value())
        return {};
    auto fields = text.substring_view(*close_paren + 1).trim_whitespace().split_view(' ');
    // After the comm field, fields[0] is state and utime/stime are fields 14 and 15.
    if (fields.size() < 13)
        return {};
    auto utime = fields[11].to_number<u64>();
    auto stime = fields[12].to_number<u64>();
    if (!utime.has_value() || !stime.has_value())
        return {};
    return *utime + *stime;
}

void EngineServer::snapshot_resolver_files()
{
    static constexpr Array<StringView, 4> resolver_paths {
        "/etc/resolv.conf"sv,
        "/etc/hosts"sv,
        "/etc/nsswitch.conf"sv,
        "/etc/host.conf"sv,
    };

    Vector<StringView> paths;
    if (auto configured = Core::Environment::get("LADYBIRD_SDE_RESOLVER_FILES"sv); configured.has_value() && !configured->is_empty()) {
        for (auto path : configured->split_view(':'))
            paths.append(path);
    } else {
        for (auto path : resolver_paths)
            paths.append(path);
    }

    m_resolver_files.clear();
    m_resolver_files.ensure_capacity(paths.size());

    for (auto path : paths) {
        ResolverFileState state;
        state.path = path.to_byte_string();

        if (auto result = Core::System::stat(path); !result.is_error()) {
            auto const& metadata = result.value();
            state.exists = true;
            state.device = metadata.st_dev;
            state.inode = metadata.st_ino;
            state.mtime = metadata.st_mtime;
            state.size = metadata.st_size;
        }

        m_resolver_files.append(move(state));
    }
}

void EngineServer::check_resolver_files()
{
    if (m_quitting)
        return;

    bool changed = false;
    Vector<ResolverFileState> current_states;
    current_states.ensure_capacity(m_resolver_files.size());

    for (auto const& previous : m_resolver_files) {
        ResolverFileState current;
        current.path = previous.path;

        if (auto result = Core::System::stat(previous.path.view()); !result.is_error()) {
            auto const& metadata = result.value();
            current.exists = true;
            current.device = metadata.st_dev;
            current.inode = metadata.st_ino;
            current.mtime = metadata.st_mtime;
            current.size = metadata.st_size;
        }

        if (current.exists != previous.exists
            || current.device != previous.device
            || current.inode != previous.inode
            || current.mtime != previous.mtime
            || current.size != previous.size) {
            changed = true;
        }

        current_states.append(move(current));
    }

    m_resolver_files = move(current_states);

    if (!changed)
        return;

    warnln("LadybirdEngine: resolver configuration changed; restarting RequestServers");
    WebView::RequestServerManager::the().restart_all_for_resolver_change();
}

void EngineServer::check_unresponsive_tabs()
{
    auto now = MonotonicTime::now();
    static auto const clock_ticks_per_second = static_cast<double>(::sysconf(_SC_CLK_TCK));
    for (auto& [id, tab] : m_tabs) {
        (void)id;
        if (!tab->waiting_for_frame || !tab->has_presented_frame || tab->unresponsive_reported)
            continue;

        auto waited_ms = (now - tab->input_wait_started).to_milliseconds();
        if (waited_ms < 1500)
            continue;

        auto ticks = web_content_cpu_ticks(tab->view->web_content_pid());
        if (!ticks.has_value())
            continue;

        auto sample_ms = (now - tab->last_cpu_sample_time).to_milliseconds();
        auto previous_ticks = tab->last_cpu_ticks;
        tab->last_cpu_sample_time = now;
        tab->last_cpu_ticks = *ticks;

        if (sample_ms <= 0 || previous_ticks == 0 || clock_ticks_per_second <= 0)
            continue;

        auto used_ticks = *ticks >= previous_ticks ? *ticks - previous_ticks : 0;
        auto cpu_fraction = (static_cast<double>(used_ticks) / clock_ticks_per_second) * 1000.0 / static_cast<double>(sample_ms);
        if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
            warnln("LadybirdEngine: tab {} unresponsive check waited={}ms cpu={}% ticks={}", tab->id, waited_ms, static_cast<int>(cpu_fraction * 100), *ticks);
        if (waited_ms >= 2500 && cpu_fraction >= 0.5) {
            tab->unresponsive_reported = true;
            tab->responsive_frame_count = 0;
            set_unresponsive(*tab, true);
        } else if (waited_ms >= 12000 && tab->wheel_events_since_frame >= 4 && cpu_fraction < 0.5) {
            // A renderer can also deadlock without spinning. Input is queued but
            // no frame arrives; after several attempts to move the page, offer a
            // hard reload rather than leaving the client area frozen.
            tab->unresponsive_reported = true;
            tab->responsive_frame_count = 0;
            set_unresponsive(*tab, true);
        }
    }
}

void EngineServer::set_unresponsive(Tab& tab, bool unresponsive)
{
    if (getenv("LADYBIRD_SDE_TRACE_FRAMES"))
        warnln("LadybirdEngine: tab {} unresponsive={}", tab.id, unresponsive);
    JsonObject event;
    event.set("type"sv, "tab_unresponsive"sv);
    event.set("tab"sv, tab.id);
    event.set("unresponsive"sv, unresponsive);
    send_event(move(event));
}

void EngineServer::handle_frame_ack(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto sequence = command.get_u64("sequence"sv);
    if (!sequence.has_value())
        return;

    for (auto& slot : tab->frame_slot_states) {
        if (slot.in_flight && slot.in_flight_sequence == *sequence) {
            slot.in_flight = false;
            slot.in_flight_sequence = 0;
        }
    }

    if (tab->frame_dirty)
        try_send_frame(*tab);
}

void EngineServer::send_event(JsonObject event)
{
    auto serialized = event.serialized();
    StringBuilder builder;
    builder.append(serialized.bytes_as_string_view());
    builder.append('\n');
    auto bytes = builder.string_view().bytes();
    m_write_buffer.append(bytes.data(), bytes.size());
    flush_writes();
}

void EngineServer::flush_writes()
{
    while (!m_write_buffer.is_empty()) {
        auto result = m_socket->write_some(m_write_buffer.span());
        if (result.is_error()) {
            if (result.error().is_errno() && (result.error().code() == EAGAIN || result.error().code() == EWOULDBLOCK)) {
                if (m_write_notifier)
                    m_write_notifier->set_enabled(true);
                return;
            }

            warnln("LadybirdEngine: socket write failed: {}", result.error());
            quit();
            return;
        }

        auto written = result.value();
        if (written == 0) {
            quit();
            return;
        }
        m_write_buffer.remove(0, written);
    }

    if (m_write_notifier)
        m_write_notifier->set_enabled(false);
}

void EngineServer::send_simple_event(StringView type, u64 tab_id)
{
    JsonObject event;
    event.set("type"sv, type);
    event.set("tab"sv, tab_id);
    send_event(move(event));
}

void EngineServer::send_tab_created(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "tab_created"sv);
    event.set("tab"sv, tab.id);
    event.set("request"sv, tab.request_id);
    event.set("width"sv, tab.viewport_size.width());
    event.set("height"sv, tab.viewport_size.height());
    if (tab.parent != 0)
        event.set("parent"sv, tab.parent);
    if (tab.window_id != 0)
        event.set("window"sv, tab.window_id);
    send_event(move(event));
}

void EngineServer::send_url(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "url"sv);
    event.set("tab"sv, tab.id);
    event.set("url"sv, tab.view->url().to_string());
    event.set("bookmarked"sv, WebView::Application::bookmark_store().is_bookmarked(tab.view->url()));
    send_event(move(event));
}

void EngineServer::send_title(Tab& tab)
{
    auto title = tab.view->title().to_utf8();
    JsonObject event;
    event.set("type"sv, "title"sv);
    event.set("tab"sv, tab.id);
    event.set("title"sv, title);
    send_event(move(event));
}

void EngineServer::send_loading_state(Tab& tab, bool loading)
{
    JsonObject event;
    event.set("type"sv, "loading"sv);
    event.set("tab"sv, tab.id);
    event.set("loading"sv, loading);
    send_event(move(event));

    send_history_state(tab);
}

void EngineServer::send_zoom_changed(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "zoom_changed"sv);
    event.set("tab"sv, tab.id);
    event.set("zoom"sv, tab.view->zoom_level());
    send_event(move(event));
}

void EngineServer::send_autocomplete_results(u64 tab_id, u64 query_id, Vector<WebView::AutocompleteSuggestion> suggestions, WebView::AutocompleteResultKind kind)
{
    JsonArray items;
    for (auto const& suggestion : suggestions) {
        JsonObject item;
        item.set("text"sv, suggestion.text);
        item.set("display"sv, WebView::autocomplete_suggestion_display_text(suggestion));
        if (suggestion.title.has_value())
            item.set("title"sv, *suggestion.title);
        if (suggestion.subtitle.has_value())
            item.set("subtitle"sv, *suggestion.subtitle);
        item.set("source"sv, static_cast<int>(suggestion.source));
        item.set("search"sv, suggestion.source == WebView::AutocompleteSuggestionSource::Search);
        items.must_append(move(item));
    }

    JsonObject event;
    event.set("type"sv, "autocomplete_results"sv);
    event.set("tab"sv, tab_id);
    event.set("request"sv, query_id);
    event.set("final"sv, kind == WebView::AutocompleteResultKind::Final);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::send_history_state(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "history"sv);
    event.set("tab"sv, tab.id);
    event.set("can_go_back"sv, tab.view->navigate_back_action().enabled());
    event.set("can_go_forward"sv, tab.view->navigate_forward_action().enabled());
    send_event(move(event));
}

void EngineServer::send_dialog(Tab& tab, StringView kind, StringView message, StringView default_text)
{
    JsonObject event;
    event.set("type"sv, "dialog"sv);
    event.set("tab"sv, tab.id);
    event.set("kind"sv, kind);
    event.set("message"sv, message);
    event.set("default"sv, default_text);
    send_event(move(event));
}

static void append_bookmark_items(JsonArray& items, Vector<WebView::BookmarkItem> const& source)
{
    for (auto const& item : source) {
        JsonObject entry;
        entry.set("id"sv, item.id);

        if (item.is_folder()) {
            auto const& folder = item.folder();
            entry.set("type"sv, "folder"sv);
            entry.set("title"sv, folder.title.value_or("Folder"_string));

            JsonArray children;
            append_bookmark_items(children, folder.children);
            entry.set("items"sv, move(children));
        } else {
            auto const& bookmark = item.bookmark();
            entry.set("type"sv, "bookmark"sv);
            entry.set("title"sv, bookmark.title.value_or(bookmark.url.to_string()));
            entry.set("url"sv, bookmark.url.to_string());
        }

        items.must_append(move(entry));
    }
}

void EngineServer::send_bookmarks()
{
    JsonArray items;
    append_bookmark_items(items, WebView::Application::bookmark_store().root_items());

    JsonObject event;
    event.set("type"sv, "bookmarks"sv);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::send_cursor(Tab& tab, Gfx::Cursor const& cursor)
{
    auto name = "Arrow"sv;
    if (auto const* standard = cursor.get_pointer<Gfx::StandardCursor>())
        name = Gfx::standard_cursor_to_string(*standard);

    JsonObject event;
    event.set("type"sv, "cursor"sv);
    event.set("tab"sv, tab.id);
    event.set("cursor"sv, name);
    send_event(move(event));
}

static StringView download_status_to_string(WebView::FileDownloader::DownloadStatus status)
{
    switch (status) {
    case WebView::FileDownloader::DownloadStatus::InProgress:
        return "in_progress"sv;
    case WebView::FileDownloader::DownloadStatus::Paused:
        return "paused"sv;
    case WebView::FileDownloader::DownloadStatus::Completed:
        return "completed"sv;
    case WebView::FileDownloader::DownloadStatus::Canceled:
        return "canceled"sv;
    case WebView::FileDownloader::DownloadStatus::Failed:
        return "failed"sv;
    }
    return "unknown"sv;
}

void EngineServer::send_context_menu(Tab& tab, StringView kind, Gfx::IntPoint position, WebView::Menu& menu)
{
    auto scale = max(tab.device_pixel_ratio, 1);
    position = { position.x() / scale, position.y() / scale };
    tab.context_menu_actions.clear();

    JsonArray items;
    size_t next_action = 0;

    Function<void(WebView::Menu&, JsonArray&)> append_menu = [&](WebView::Menu& current_menu, JsonArray& destination) {
        for (auto& item : current_menu.items()) {
            item.visit(
                [&](NonnullRefPtr<WebView::Action>& action) {
                    if (!action->visible())
                        return;

                    JsonObject entry;
                    entry.set("text"sv, action->text());
                    entry.set("action"sv, static_cast<u64>(next_action++));
                    entry.set("enabled"sv, action->enabled());
                    entry.set("checkable"sv, action->is_checkable());
                    if (action->is_checkable())
                        entry.set("checked"sv, action->checked());

                    tab.context_menu_actions.append(action);
                    destination.must_append(move(entry));
                },
                [&](NonnullRefPtr<WebView::Menu>& submenu) {
                    if (!submenu->visible())
                        return;

                    JsonObject entry;
                    entry.set("title"sv, submenu->title());
                    JsonArray submenu_items;
                    append_menu(*submenu, submenu_items);
                    entry.set("items"sv, move(submenu_items));
                    destination.must_append(move(entry));
                },
                [&](WebView::Separator) {
                    JsonObject entry;
                    entry.set("separator"sv, true);
                    destination.must_append(move(entry));
                });
        }
    };

    append_menu(menu, items);

    JsonObject event;
    event.set("type"sv, "context_menu"sv);
    event.set("tab"sv, tab.id);
    event.set("kind"sv, kind);
    event.set("x"sv, position.x());
    event.set("y"sv, position.y());
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::handle_context_menu_action(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto action_index = command.get_u64("action"sv);
    if (!action_index.has_value())
        return;

    auto actions = move(tab->context_menu_actions);
    if (*action_index >= actions.size())
        return;

    actions[*action_index]->activate();
}

void EngineServer::send_permission_request(Tab& tab, Utf16String const& name, URL::Origin const& origin, u64 request_id)
{
    JsonObject event;
    event.set("type"sv, "permission"sv);
    event.set("tab"sv, tab.id);
    event.set("request"sv, request_id);
    event.set("name"sv, name.to_utf8());
    event.set("origin"sv, origin.serialize());
    send_event(move(event));
}

void EngineServer::handle_permission_response(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto request_id = command.get_u64("request"sv);
    if (!request_id.has_value())
        return;

    auto callback = tab->pending_permissions.take(*request_id);
    if (callback.has_value())
        (*callback)(command.get_bool("granted"sv).value_or(false));
}

void EngineServer::send_file_picker_request(Tab& tab, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    JsonArray filters;
    for (auto const& filter : accepted_file_types.filters) {
        filter.visit(
            [&](Web::HTML::FileFilter::FileType type) {
                switch (type) {
                case Web::HTML::FileFilter::FileType::Audio:
                    filters.must_append("Audio files"sv);
                    break;
                case Web::HTML::FileFilter::FileType::Image:
                    filters.must_append("Image files"sv);
                    break;
                case Web::HTML::FileFilter::FileType::Video:
                    filters.must_append("Video files"sv);
                    break;
                }
            },
            [&](Web::HTML::FileFilter::MimeType const& mime_type) {
                filters.must_append(mime_type.value.to_utf8());
            },
            [&](Web::HTML::FileFilter::Extension const& extension) {
                filters.must_append(String::formatted("*.{}", extension.value.to_utf8()).release_value());
            });
    }

    JsonObject event;
    event.set("type"sv, "file_picker"sv);
    event.set("tab"sv, tab.id);
    event.set("multiple"sv, allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes);
    event.set("filters"sv, move(filters));
    send_event(move(event));
}

void EngineServer::handle_file_picker_response(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    Vector<Web::HTML::SelectedFile> selected_files;

    auto append_path = [&](String const& path) {
        if (path.is_empty())
            return;
        if (auto file = WebView::create_selected_file(path.to_byte_string()); file.is_error())
            warnln("LadybirdEngine: could not open selected file '{}': {}", path.bytes_as_string_view(), file.error());
        else
            selected_files.append(file.release_value());
    };

    if (auto paths = command.get_array("paths"sv); paths.has_value()) {
        for (auto const& value : paths->values()) {
            if (value.is_string())
                append_path(value.as_string());
        }
    } else if (auto path = command.get_string("path"sv); path.has_value()) {
        append_path(*path);
    }

    tab->view->file_picker_closed(move(selected_files));
}

void EngineServer::send_select_dropdown(Tab& tab, Gfx::IntPoint position, i32 minimum_width, Vector<Web::HTML::SelectItem> const& items)
{
    auto option_to_json = [](Web::HTML::SelectItemOption const& option) {
        JsonObject object;
        object.set("id"sv, static_cast<u64>(option.id));
        object.set("label"sv, option.label.to_utf8());
        object.set("selected"sv, option.selected);
        object.set("disabled"sv, option.disabled);
        return object;
    };

    JsonArray serialized_items;
    for (auto const& item : items) {
        if (item.has<Web::HTML::SelectItemOption>()) {
            serialized_items.must_append(option_to_json(item.get<Web::HTML::SelectItemOption>()));
        } else if (item.has<Web::HTML::SelectItemOptionGroup>()) {
            auto const& group = item.get<Web::HTML::SelectItemOptionGroup>();
            JsonArray group_items;
            for (auto const& option : group.items)
                group_items.must_append(option_to_json(option));

            JsonObject object;
            object.set("group"sv, group.label.to_utf8());
            object.set("items"sv, move(group_items));
            serialized_items.must_append(move(object));
        } else {
            JsonObject object;
            object.set("separator"sv, true);
            serialized_items.must_append(move(object));
        }
    }

    auto scale = max(tab.device_pixel_ratio, 1);
    JsonObject event;
    event.set("type"sv, "select_dropdown"sv);
    event.set("tab"sv, tab.id);
    event.set("x"sv, position.x() / scale);
    event.set("y"sv, position.y() / scale);
    event.set("min_width"sv, minimum_width / scale);
    event.set("items"sv, move(serialized_items));
    send_event(move(event));
}

void EngineServer::handle_select_dropdown_closed(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto selected = command.get_u64("selected"sv);
    if (selected.has_value())
        tab->view->select_dropdown_closed(static_cast<u32>(*selected));
    else
        tab->view->select_dropdown_closed({});
}

JsonObject EngineServer::download_to_json(WebView::FileDownloader::Download const& download)
{
    JsonObject object;
    object.set("id"sv, download.id);
    object.set("url"sv, download.url.to_string());
    object.set("destination"sv, String::from_byte_string(download.destination.string()).release_value());
    object.set("status"sv, download_status_to_string(download.status));
    object.set("downloaded"sv, download.downloaded_size);
    object.set("can_resume"sv, download.can_resume);
    object.set("private"sv, download.is_private == WebView::IsPrivate::Yes);
    if (download.total_size.has_value())
        object.set("total"sv, *download.total_size);
    if (auto progress = download.progress(); progress.has_value())
        object.set("progress"sv, *progress);
    if (download.error.has_value())
        object.set("error"sv, *download.error);
    return object;
}

void EngineServer::send_session_plan()
{
    JsonArray windows;

    if (!Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv) && WebView::Application::settings().restore_previous_session()) {
        if (auto* store = WebView::Application::session_store(WebView::IsPrivate::No)) {
            for (auto const& restored_window : store->take_launch_plan()) {
                JsonArray tabs;
                for (auto const& tab : restored_window.tabs) {
                    JsonObject entry;
                    entry.set("session_tab"sv, static_cast<u64>(tab.tab_id));
                    entry.set("url"sv, tab.url.to_string());
                    entry.set("pinned"sv, tab.pinned);
                    tabs.must_append(move(entry));
                }

                JsonObject window;
                window.set("window"sv, static_cast<u64>(restored_window.window_id));
                window.set("active"sv, static_cast<u64>(restored_window.active_tab_index));
                window.set("tabs"sv, move(tabs));
                if (restored_window.geometry.has_value()) {
                    auto const& geometry = *restored_window.geometry;
                    window.set("x"sv, geometry.x);
                    window.set("y"sv, geometry.y);
                    window.set("width"sv, geometry.width);
                    window.set("height"sv, geometry.height);
                    window.set("maximized"sv, geometry.maximized);
                }
                windows.must_append(move(window));
            }
        }
    }

    JsonObject event;
    event.set("type"sv, "session_plan"sv);
    event.set("windows"sv, move(windows));
    send_event(move(event));
}

void EngineServer::send_downloads()
{
    JsonArray items;
    for (auto const& download : WebView::Application::the().file_downloader().downloads())
        items.must_append(download_to_json(download));

    JsonObject event;
    event.set("type"sv, "downloads"sv);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::send_download_event(StringView type, WebView::FileDownloader::Download const& download)
{
    JsonObject event;
    event.set("type"sv, type);
    event.set("download"sv, download_to_json(download));
    send_event(move(event));
}

void EngineServer::handle_download_action(JsonObject const& command)
{
    auto action = command.get_string("action"sv);
    auto download_id = command.get_u64("id"sv);
    if (!action.has_value() || !download_id.has_value())
        return;

    auto& downloader = WebView::Application::the().file_downloader();
    auto action_name = action->bytes_as_string_view();
    if (action_name == "clear"sv) {
        if (command.get_bool("private"sv).value_or(false))
            (void)downloader.prune_inactive_downloads(WebView::IsPrivate::Yes);
        else
            (void)downloader.prune_inactive_downloads(WebView::IsPrivate::No);
        return;
    }

    auto download = downloader.download(*download_id);
    if (!download.has_value())
        return;

    if (action_name == "cancel"sv) {
        downloader.cancel_download(*download_id);
    } else if (action_name == "pause"sv) {
        downloader.pause_download(*download_id);
    } else if (action_name == "resume"sv) {
        downloader.resume_download(*download_id);
    } else if (action_name == "open"sv) {
        WebView::Application::the().open_download(*download);
    } else if (action_name == "show_in_folder"sv) {
        WebView::Application::the().show_download_in_folder(*download);
    }
}

void EngineServer::download_added(WebView::FileDownloader::Download const& download)
{
    send_download_event("download_added"sv, download);
}

void EngineServer::download_updated(WebView::FileDownloader::Download const& download)
{
    send_download_event("download_updated"sv, download);
}

void EngineServer::download_removed(u64 id, WebView::IsPrivate is_private)
{
    JsonObject event;
    event.set("type"sv, "download_removed"sv);
    event.set("id"sv, id);
    event.set("private"sv, is_private == WebView::IsPrivate::Yes);
    send_event(move(event));
}


}
