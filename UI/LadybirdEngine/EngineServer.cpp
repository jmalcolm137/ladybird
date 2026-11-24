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
#include <LibAudio/Audibility.h>
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
#include <LibWebCommon/HTML/WebViewHints.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/Page/DragData.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/BookmarkStore.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/ProcessSnapshot.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>
#include <Shared/EngineProtocol.h>
#include <LibCore/Process.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
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
    Function<void(WebView::ContextMenuRequest)> on_context_menu_request;
    Function<void(WebView::SessionHistorySnapshot const&, URL::URL const&)> on_session_history_change;

    static NonnullOwnPtr<EngineHeadlessWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, WebView::IsPrivate is_private)
    {
        auto view = adopt_own(*new EngineHeadlessWebView(move(theme), viewport_size, is_private));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    static NonnullOwnPtr<EngineHeadlessWebView> create_child(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, WebView::IsPrivate is_private, WebView::CanonicalTraversable& traversable)
    {
        auto view = adopt_own(*new EngineHeadlessWebView(move(theme), viewport_size, is_private));
        view->initialize_tab(Web::HTML::VisibilityState::Visible, traversable);
        return view;
    }

protected:
    EngineHeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, WebView::IsPrivate is_private)
        : HeadlessWebView(move(theme), viewport_size, is_private)
    {
    }

    virtual WebView::ViewImplementation* create_view_for_new_tab_or_window(WebView::IsPrivate) override { return nullptr; }

    virtual void did_request_context_menu(WebView::ContextMenuRequest request) override
    {
        if (on_context_menu_request)
            on_context_menu_request(move(request));
    }

    virtual void did_change_session_history(WebView::SessionHistorySnapshot const& history, URL::URL const& url) override
    {
        if (on_session_history_change)
            on_session_history_change(history, url);
    }
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
    if (m_crash_report_check_timer)
        m_crash_report_check_timer->stop();
    if (m_resolver_watch_timer)
        m_resolver_watch_timer->stop();
    if (m_write_notifier) {
        m_write_notifier->close();
        m_write_notifier.clear();
    }
    for (auto const& pending : m_pending_write_fds) {
        if (pending.fd >= 0)
            ::close(pending.fd);
    }
    m_pending_write_fds.clear();
    shutdown_certificate_inspector();
    m_certificate_request_generations.clear();
    for (auto& [id, tab] : m_tabs) {
        (void)id;
        destroy_frame_buffer(*tab);
    }
}

static StringView website_color_scheme_to_string(WebView::ThemePreference preference)
{
    switch (preference) {
    case WebView::ThemePreference::System:
        return "auto"sv;
    case WebView::ThemePreference::Light:
        return "light"sv;
    case WebView::ThemePreference::Dark:
        return "dark"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<WebView::ThemePreference> website_color_scheme_from_string(StringView value)
{
    if (value == "auto"sv)
        return WebView::ThemePreference::System;
    if (value == "light"sv)
        return WebView::ThemePreference::Light;
    if (value == "dark"sv)
        return WebView::ThemePreference::Dark;
    return {};
}

static StringView preferred_contrast_to_string(Web::CSS::PreferredContrast contrast)
{
    switch (contrast) {
    case Web::CSS::PreferredContrast::Auto:
        return "auto"sv;
    case Web::CSS::PreferredContrast::Less:
        return "less"sv;
    case Web::CSS::PreferredContrast::More:
        return "more"sv;
    case Web::CSS::PreferredContrast::NoPreference:
        return "no-preference"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<Web::CSS::PreferredContrast> preferred_contrast_from_string(StringView value)
{
    if (value == "auto"sv)
        return Web::CSS::PreferredContrast::Auto;
    if (value == "less"sv)
        return Web::CSS::PreferredContrast::Less;
    if (value == "more"sv)
        return Web::CSS::PreferredContrast::More;
    if (value == "no-preference"sv)
        return Web::CSS::PreferredContrast::NoPreference;
    return {};
}

static StringView preferred_motion_to_string(Web::CSS::PreferredMotion motion)
{
    switch (motion) {
    case Web::CSS::PreferredMotion::Auto:
        return "auto"sv;
    case Web::CSS::PreferredMotion::NoPreference:
        return "no-preference"sv;
    case Web::CSS::PreferredMotion::Reduce:
        return "reduce"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<Web::CSS::PreferredMotion> preferred_motion_from_string(StringView value)
{
    if (value == "auto"sv)
        return Web::CSS::PreferredMotion::Auto;
    if (value == "no-preference"sv)
        return Web::CSS::PreferredMotion::NoPreference;
    if (value == "reduce"sv)
        return Web::CSS::PreferredMotion::Reduce;
    return {};
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
        release_idle_hidden_frame_buffers();
    });
    m_crash_report_check_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1000), [this] {
        check_crash_reports();
    });

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

    // The chrome builds its user-agent picker from the same presets the reference frontend uses. The current value is
    // the application's, so a custom override set earlier is reflected too.
    JsonObject user_agents_event;
    user_agents_event.set("type"sv, "user_agents"sv);
    user_agents_event.set("current"sv, EngineApplication::the().user_agent_string());
    JsonArray user_agent_presets;
    JsonObject default_user_agent;
    default_user_agent.set("name"sv, "Default"sv);
    default_user_agent.set("value"sv, Web::default_user_agent);
    user_agent_presets.must_append(move(default_user_agent));
    for (auto const& [name, value] : WebView::user_agents) {
        JsonObject preset;
        preset.set("name"sv, name);
        preset.set("value"sv, value);
        user_agent_presets.must_append(move(preset));
    }
    user_agents_event.set("agents"sv, move(user_agent_presets));
    send_event(move(user_agents_event));

    JsonObject navigator_modes_event;
    navigator_modes_event.set("type"sv, "navigator_compatibility_modes"sv);
    navigator_modes_event.set("current"sv, WebView::navigator_compatibility_mode_to_string(EngineApplication::the().navigator_compatibility_mode()));
    JsonArray navigator_modes;
    auto add_navigator_mode = [&](StringView name, Web::NavigatorCompatibilityMode mode) {
        JsonObject entry;
        entry.set("name"sv, name);
        entry.set("value"sv, WebView::navigator_compatibility_mode_to_string(mode));
        navigator_modes.must_append(move(entry));
    };
    add_navigator_mode("Chrome"sv, Web::NavigatorCompatibilityMode::Chrome);
    add_navigator_mode("Gecko"sv, Web::NavigatorCompatibilityMode::Gecko);
    add_navigator_mode("WebKit"sv, Web::NavigatorCompatibilityMode::WebKit);
    navigator_modes_event.set("modes"sv, move(navigator_modes));
    send_event(move(navigator_modes_event));

    send_view_preferences();

    check_crash_reports();

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
    EngineApplication::the().flush_session_store();
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

        static constexpr size_t max_ipc_buffer_bytes = 64 * MiB;
        if (m_read_buffer.size() - m_read_offset + bytes.size() > max_ipc_buffer_bytes) {
            warnln("LadybirdEngine: IPC read buffer limit exceeded");
            quit(1);
            return;
        }

        m_read_buffer.append(bytes.data(), bytes.size());

        while (m_read_offset < m_read_buffer.size()) {
            auto remaining = m_read_buffer.span().slice(m_read_offset);
            auto const* newline = static_cast<u8 const*>(::memchr(remaining.data(), '\n', remaining.size()));
            if (!newline)
                break;

            auto line_length = static_cast<size_t>(newline - remaining.data());
            auto line_view = StringView { reinterpret_cast<char const*>(remaining.data()), line_length };
            if (!line_view.is_empty() && line_view.ends_with('\r'))
                line_view = line_view.substring_view(0, line_view.length() - 1);

            // Advance past the command before dispatching it. Some commands can
            // run nested event loops, during which the socket notifier can call
            // read_available again.
            auto line = String::from_utf8(line_view);
            m_read_offset += line_length + 1;
            if (m_read_offset == m_read_buffer.size()) {
                m_read_buffer.clear_with_capacity();
                m_read_offset = 0;
            } else if (m_read_offset >= 4096 && m_read_offset * 2 >= m_read_buffer.size()) {
                m_read_buffer.remove(0, m_read_offset);
                m_read_offset = 0;
            }
            if (line.is_error()) {
                warnln("LadybirdEngine: could not decode command: {}", line.error());
                continue;
            }
            dispatch_line(line.value().bytes_as_string_view());
        }

        if (m_read_buffer.size() - m_read_offset > 4 * 1024 * 1024) {
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
    } else if (*type == "set_user_agent"sv) {
        handle_set_user_agent(command);
    } else if (*type == "set_navigator_compatibility_mode"sv) {
        handle_set_navigator_compatibility_mode(command);
    } else if (*type == "set_website_color_scheme"sv) {
        handle_set_website_color_scheme(command);
    } else if (*type == "set_preferred_contrast"sv) {
        handle_set_preferred_contrast(command);
    } else if (*type == "set_preferred_motion"sv) {
        handle_set_preferred_motion(command);
    } else if (*type == "request_processes"sv) {
        send_processes();
    } else if (*type == "end_process"sv) {
        handle_end_process(command);
    } else if (*type == "crash_report_open"sv) {
        handle_crash_report_open(command);
    } else if (*type == "crash_report_next"sv) {
        handle_crash_report_next();
    } else if (*type == "crash_report_text"sv) {
        handle_crash_report_text();
    } else if (*type == "crash_report_send"sv) {
        handle_crash_report_send(command);
    } else if (*type == "crash_report_cancel"sv) {
        handle_crash_report_cancel();
    } else if (*type == "crash_report_close"sv) {
        handle_crash_report_close();
    } else if (*type == "crash_report_dismiss"sv) {
        handle_crash_report_dismiss();
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
        } else if (*type == "drag"sv) {
            handle_drag(*tab_id, command);
        } else if (*type == "compat_report_begin"sv) {
            handle_compat_report_begin(*tab_id);
        } else if (*type == "compat_report_diagnostics"sv) {
            handle_compat_report_diagnostics(*tab_id, command);
        } else if (*type == "compat_report_send"sv) {
            handle_compat_report_send(*tab_id, command);
        } else if (*type == "compat_report_retry"sv) {
            handle_compat_report_retry(*tab_id);
        } else if (*type == "compat_report_cancel"sv) {
            handle_compat_report_cancel(*tab_id);
        } else if (*type == "compat_report_close"sv) {
            handle_compat_report_close(*tab_id);
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
    auto view = EngineHeadlessWebView::create_child(m_theme, parent.viewport_size.to_type<Web::DevicePixels>(), parent.view->is_private(), traversable);

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
    tab.autocomplete = new WebView::Autocomplete(EngineApplication::the(), is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);

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

    tab.view->on_tab_indicator_change = [this, tab_id](WebView::TabIndicator indicator, bool) {
        if (indicator != WebView::TabIndicator::PlayingAudio)
            return;
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
            event.set("data"sv, String {});
            send_event(move(event));
            return;
        }

        auto png = Gfx::PNGWriter::encode(*favicon);
        if (png.is_error() || png.value().size() > 64 * KiB)
            return;

        auto encoded = AK::encode_base64(png.value().bytes());
        if (encoded.is_error())
            return;

        event.set("data"sv, encoded.release_value());
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
        Core::deferred_invoke([weak_this = make_weak_ptr(), tab_id] {
            if (auto* self = weak_this.ptr())
                self->destroy_tab(tab_id);
        });
    };

    tab.view->on_web_content_crashed = [this, tab_id](auto) {
        if (tab_for_id(tab_id))
            send_simple_event("tab_crashed"sv, tab_id);
        schedule_crash_report_check();
    };

    static_cast<EngineHeadlessWebView&>(*tab.view).on_context_menu_request = [this, tab_id](WebView::ContextMenuRequest request) {
        handle_context_menu_request(tab_id, move(request));
    };

    static_cast<EngineHeadlessWebView&>(*tab.view).on_session_history_change = [this, tab_id](WebView::SessionHistorySnapshot const& history, URL::URL const& url) {
        auto* tab = tab_for_id(tab_id);
        if (!tab || !tab->session_tab_id.has_value())
            return;
        if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No))
            store->update_tab_state({ .tab_id = *tab->session_tab_id, .history = history, .url = url });
    };

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

    tab.view->on_start_drag = [this, tab_id](Web::DragData drag_data) {
        send_drag_start(tab_id, move(drag_data));
    };

    tab.view->on_request_permission = [this, tab_id](WebView::PermissionPromptRequest const& request, WebView::PermissionPromptCallback on_decided) {
        auto* tab = tab_for_id(tab_id);
        if (!tab) {
            on_decided(Web::Permissions::PromptOutcome::Cancelled);
            return;
        }

        auto request_id = m_next_permission_id++;
        tab->pending_permissions.set(request_id, Tab::PendingPermission { request, move(on_decided) });
        send_permission_request(*tab, request, request_id);
    };

    // The engine withdraws unanswered prompts when the page goes away. Drop the matching callbacks; the engine
    // settles them itself with PromptOutcome::Ignored.
    tab.view->on_cancel_permission_prompt = [this, tab_id](WebView::PermissionPromptRequest const& request) {
        auto* tab = tab_for_id(tab_id);
        if (!tab)
            return;
        tab->pending_permissions.remove_all_matching([&](auto&, auto& value) {
            return value.request.is_same_request_as(request);
        });
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
    m_certificate_request_generations.remove(tab_id);

    auto tab = m_tabs.take(tab_id);
    if (!tab.has_value())
        return;

    destroy_frame_buffer(**tab);

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
    auto width = clamp(command.get_i64("width"sv).value_or(1024), static_cast<i64>(1), static_cast<i64>(16384));
    auto height = clamp(command.get_i64("height"sv).value_or(768), static_cast<i64>(1), static_cast<i64>(16384));
    auto is_private = command.get_bool("private"sv).value_or(false);
    auto device_pixel_ratio = clamp(static_cast<int>(command.get_i64("scale"sv).value_or(1)), 1, 4);

    auto request_id = command.get_u64("request"sv).value_or(0);
    auto window_id = command.get_u64("window"sv).value_or(0);
    auto session_tab = command.get_u64("session_tab"sv);
    auto session_window = command.get_u64("session_window"sv);

    auto& tab = create_tab({ static_cast<int>(width), static_cast<int>(height) }, is_private, request_id, 0, window_id, device_pixel_ratio);

    auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No);

    if (session_window.has_value() && *session_window != 0)
        m_session_windows.set(window_id, static_cast<Ladybird::SessionWindowId>(*session_window));

    if (!is_private && store && !Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv)) {
        if (session_tab.has_value() && *session_tab != 0) {
            auto tab_id = static_cast<Ladybird::SessionTabId>(*session_tab);
            tab.session_tab_id = tab_id;
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
                .is_active = Ladybird::SessionStore::IsActive::No,
            });
            if (!tab_id.is_error()) {
                tab.session_tab_id = tab_id.value();
            }
        }
    }

    if (tab.session_tab_id.has_value())
        tab.view->set_reports_session_history(true);

    if (tab.session_tab_id.has_value() && store) {
        auto history = store->load_tab_history(*tab.session_tab_id);
        if (history.has_value() && !tab.view->restore_session_history_from_snapshot(history.release_value()).is_error()) {
            // The snapshot's current entry is authoritative for back/forward history, but the session tab's active URL
            // is the page the user last saw. A snapshot written just before a navigation can lag it by one entry.
            if (auto requested = command.get_string("url"sv); requested.has_value() && !requested->is_empty()) {
                if (auto parsed = URL::Parser::basic_parse(requested->bytes_as_string_view()); parsed.has_value() && tab.view->url() != *parsed)
                    tab.view->load_from_browser_ui(*parsed);
            }
            return;
        }
    }

    if (auto url = command.get_string("url"sv); url.has_value() && !url->is_empty()) {
        auto settings = WebView::Application::settings().search_engine_settings();
        if (auto parsed = WebView::sanitize_url(url->bytes_as_string_view(), settings.engine); parsed.has_value())
            tab.view->load_from_browser_ui(*parsed);
        else
            tab.view->load_from_browser_ui(WebView::Application::settings().new_tab_page_url());
    } else {
        tab.view->load_from_browser_ui(WebView::Application::settings().new_tab_page_url());
    }
}

void EngineServer::handle_close_window(JsonObject const& command)
{
    auto session_window = command.get_u64("session_window"sv);
    if (!session_window.has_value() || *session_window == 0)
        return;

    auto window_id = static_cast<Ladybird::SessionWindowId>(*session_window);
    if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No)) {
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

    auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No);
    if (!store)
        return;

    store->window_geometry_changed(
        static_cast<Ladybird::SessionWindowId>(*session_window),
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
    auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No);
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
        .window_id = static_cast<Ladybird::SessionWindowId>(*session_window),
        .initial_url = parsed.release_value(),
        .insertion_index = {},
        .is_active = Ladybird::SessionStore::IsActive::No,
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

Ladybird::SessionWindowId EngineServer::session_window_for(u64 frontend_window_id, bool create)
{
    if (auto it = m_session_windows.find(frontend_window_id); it != m_session_windows.end())
        return it->value;

    if (!create)
        return 0;

    auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No);
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
        if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No))
            store->active_tab_changed(*tab->session_tab_id);
    }

    auto now = MonotonicTime::now();
    bool any_hidden_with_buffer = false;
    for (auto& [id, other_tab] : m_tabs) {
        bool visible = false;
        if (other_tab->window_id != 0)
            visible = m_window_active_tabs.get(other_tab->window_id).value_or(0) == id;
        else
            visible = id == tab_id;
        other_tab->view->set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
        if (visible) {
            other_tab->hidden_since.clear();
            if (!other_tab->frame_mapping) {
                other_tab->frame_dirty = true;
                try_send_frame(*other_tab);
            }
        } else {
            if (!other_tab->hidden_since.has_value())
                other_tab->hidden_since = now;
            if (other_tab->frame_mapping)
                any_hidden_with_buffer = true;
        }
    }
    if (any_hidden_with_buffer)
        ensure_maintenance_timer_running();
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
        tab.view->load_from_browser_ui(WebView::Application::settings().new_tab_page_url());
        return;
    }

    if (user_input) {
        auto settings = WebView::Application::settings().search_engine_settings();
        if (auto parsed = WebView::sanitize_url(url.bytes_as_string_view(), settings.engine); parsed.has_value())
            tab.view->load_from_browser_ui(*parsed);
        else
            tab.view->load_from_browser_ui(WebView::Application::settings().new_tab_page_url());
    } else if (auto parsed = URL::Parser::basic_parse(url.bytes_as_string_view()); parsed.has_value()) {
        tab.view->load_from_browser_ui(*parsed);
    }
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

    if (!m_cached_autocomplete_bookmarks.has_value()) {
        Vector<WebView::AutocompleteBookmark> bookmarks;
        Function<void(Vector<Ladybird::BookmarkItem> const&, Optional<String>)> append_bookmarks = [&](Vector<Ladybird::BookmarkItem> const& items, Optional<String> folder) {
            for (auto const& item : items) {
                if (item.is_folder()) {
                    auto next_folder = folder;
                    if (item.folder().title.has_value() && !item.folder().title->is_empty()) {
                        next_folder = folder.has_value()
                            ? MUST(String::formatted("{} / {}", *folder, *item.folder().title))
                            : *item.folder().title;
                    }
                    append_bookmarks(item.folder().children, move(next_folder));
                } else {
                    bookmarks.append({
                        .url = item.bookmark().url.to_string(),
                        .title = item.bookmark().title,
                        .folder = folder,
                        .favicon_png = {},
                    });
                }
            }
        };
        append_bookmarks(EngineApplication::the().bookmark_store().root_items(), {});
        m_cached_autocomplete_bookmarks = move(bookmarks);
    }

    auto suggestions = WebView::rank_bookmark_suggestions(
        query->bytes_as_string_view(),
        *m_cached_autocomplete_bookmarks,
        WebView::autocomplete_preliminary_limit(WebView::default_autocomplete_suggestion_limit));

    tab->autocomplete->query_autocomplete_engine(*request_id, *query, WebView::default_autocomplete_suggestion_limit, move(suggestions));
}

void EngineServer::handle_resize(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto width = static_cast<int>(clamp(command.get_i64("width"sv).value_or(1), static_cast<i64>(1), static_cast<i64>(16384)));
    auto height = static_cast<int>(clamp(command.get_i64("height"sv).value_or(1), static_cast<i64>(1), static_cast<i64>(16384)));
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

void EngineServer::handle_dialog_response(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("kind"sv);
    auto kind_name = kind.has_value() ? kind->bytes_as_string_view() : ""sv;
    auto accepted = command.get_bool("accept"sv).value_or(true);
    tab->pending_dialog_kind.clear();

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
        if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No)) {
            (void)store->tab_closed({
                .tab_id = *tab->session_tab_id,
                .closed_at = UnixDateTime::now(),
                .keep_for_reopening = Ladybird::SessionStore::KeepForReopening::Yes,
            });
        }
    }

    tab->closing = true;
    if (tab->pending_dialog_kind.has_value()) {
        auto kind = tab->pending_dialog_kind.release_value();
        if (kind == "alert"sv)
            tab->view->alert_closed();
        else if (kind == "confirm"sv)
            tab->view->confirm_closed(false);
        else if (kind == "prompt"sv)
            tab->view->prompt_closed({});
    }
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

    auto& store = EngineApplication::the().bookmark_store();
    auto url = tab->view->url();
    if (auto bookmark = store.find_bookmark_by_url(url); bookmark.has_value()) {
        store.remove_item(bookmark->id);
    } else {
        store.add_bookmark(url, tab->view->title().to_utf8(), {});
    }

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
    auto& store = EngineApplication::the().history_store();
    for (auto const& entry : store.list_entries({}, 0, limit)) {
        JsonObject item;
        item.set("url"sv, entry.url);
        item.set("title"sv, entry.title.value_or(entry.url));
        items.must_append(move(item));
    }

    JsonObject event;
    event.set("type"sv, "history_recent"sv);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::handle_history_clear()
{
    EngineApplication::the().history_store().remove_entries_accessed_since(UnixDateTime::earliest());

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
        EngineApplication::the().bookmark_store().add_bookmark(url.release_value(), move(title), {}, *parent);
    else
        EngineApplication::the().bookmark_store().add_bookmark(url.release_value(), move(title), {});
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
    EngineApplication::the().bookmark_store().edit_bookmark(*id, url.release_value(), move(title));
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

    EngineApplication::the().bookmark_store().remove_item(*id);
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
    auto& store = EngineApplication::the().history_store();
    for (auto const& entry : store.list_entries(query.bytes_as_string_view(), 0, limit)) {
        JsonObject item;
        item.set("url"sv, entry.url);
        item.set("title"sv, entry.title.value_or(entry.url));
        items.must_append(move(item));
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
        EngineApplication::the().bookmark_store().add_folder(move(title), parent);
    } else {
        EngineApplication::the().bookmark_store().add_folder(move(title), {});
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

    EngineApplication::the().bookmark_store().edit_folder(*id, move(title));
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
        EngineApplication::the().bookmark_store().move_item(*id, parent, index);
    } else {
        EngineApplication::the().bookmark_store().move_item(*id, {}, index);
    }
    send_bookmarks();
}

void EngineServer::handle_pin_tab(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->session_tab_id.has_value())
        return;

    if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No))
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

void EngineServer::send_audio_state(Tab& tab)
{
    JsonObject event;
    event.set("type"sv, "audio"sv);
    event.set("tab"sv, tab.id);
    event.set("playing"sv, tab.view->is_playing_audio());
    event.set("muted"sv, tab.view->page_mute_state() == Web::HTML::MuteState::Muted);
    send_event(move(event));
}

void EngineServer::handle_clipboard_copy(u64 tab_id, bool cut)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto promise = cut ? tab->view->cut_selected_text() : tab->view->selected_text();
    promise->when_resolved([weak_this = make_weak_ptr(), tab_id](ByteString& text) {
        auto* self = weak_this.ptr();
        if (!self || text.is_empty())
            return;

        JsonObject event;
        event.set("type"sv, "clipboard"sv);
        event.set("tab"sv, tab_id);
        event.set("text"sv, text.view());
        self->send_event(move(event));
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

    tab->view->paste_from_clipboard(Web::Clipboard::PasteMode::AllMimeTypes);
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

void EngineServer::send_view_preferences()
{
    JsonObject event;
    event.set("type"sv, "view_preferences"sv);
    event.set("color_scheme"sv, website_color_scheme_to_string(WebView::Application::settings().appearance().website_color_scheme));
    event.set("contrast"sv, preferred_contrast_to_string(EngineApplication::the().preferred_contrast()));
    event.set("motion"sv, preferred_motion_to_string(EngineApplication::the().preferred_motion()));
    send_event(move(event));
}

void EngineServer::send_processes()
{
    auto processes = WebView::process_snapshots(WebView::Application::process_manager());

    JsonArray items;
    double total_cpu = 0;
    u64 total_memory = 0;
    for (auto const& process : processes) {
        total_cpu += process.cpu_percent;
        total_memory += process.memory_usage_bytes;

        JsonObject item;
        item.set("child"sv, process.child_id);
        item.set("pid"sv, static_cast<i64>(process.pid));
        item.set("type"sv, WebView::process_name_from_type(process.type));
        if (process.title.has_value())
            item.set("title"sv, process.title->to_utf8());
        if (process.site_url.has_value())
            item.set("site"sv, process.site_url->to_string());
        item.set("cpu"sv, process.cpu_percent);
        item.set("memory"sv, process.memory_usage_bytes);
        if (process.owner_child_id.has_value())
            item.set("owner"sv, *process.owner_child_id);

        if (process.page.has_value())
            item.set("page"sv, process.page->url.to_string());

        JsonArray tabs;
        for (auto const& tab : process.tabs) {
            JsonObject entry;
            entry.set("title"sv, tab.title.to_utf8());
            entry.set("url"sv, tab.page.url.to_string());
            tabs.must_append(move(entry));
        }
        item.set("tabs"sv, move(tabs));
        item.set("frames"sv, static_cast<u64>(process.frames.size()));
        items.must_append(move(item));
    }

    JsonObject event;
    event.set("type"sv, "processes"sv);
    event.set("items"sv, move(items));
    event.set("total_cpu"sv, total_cpu);
    event.set("total_memory"sv, total_memory);
    send_event(move(event));
}

void EngineServer::handle_end_process(JsonObject const& command)
{
    auto child_id = command.get_u64("child"sv);
    if (!child_id.has_value())
        return;

    auto result = WebView::Application::process_manager().terminate_child(*child_id, Core::Process::TerminationMode::Forceful);
    if (result.is_error()) {
        JsonObject event;
        event.set("type"sv, "process_error"sv);
        event.set("message"sv, result.error().string_literal());
        send_event(move(event));
    }
    send_processes();
}

void EngineServer::handle_set_user_agent(JsonObject const& command)
{
    auto value = command.get_string("value"sv);
    if (!value.has_value())
        return;

    EngineApplication::the().set_user_agent_string(*value);

    JsonObject event;
    event.set("type"sv, "user_agent_changed"sv);
    event.set("value"sv, EngineApplication::the().user_agent_string());
    send_event(move(event));
}

void EngineServer::handle_set_navigator_compatibility_mode(JsonObject const& command)
{
    auto value = command.get_string("value"sv);
    if (!value.has_value())
        return;

    auto mode = WebView::navigator_compatibility_mode_from_string(*value);
    if (!mode.has_value())
        return;

    EngineApplication::the().set_navigator_compatibility_mode(*mode);

    JsonObject event;
    event.set("type"sv, "navigator_compatibility_mode_changed"sv);
    event.set("value"sv, WebView::navigator_compatibility_mode_to_string(*mode));
    send_event(move(event));
}

void EngineServer::handle_set_website_color_scheme(JsonObject const& command)
{
    auto value = command.get_string("value"sv);
    if (!value.has_value())
        return;

    auto preference = website_color_scheme_from_string(*value);
    if (!preference.has_value())
        return;

    auto appearance = WebView::Application::settings().appearance();
    appearance.website_color_scheme = *preference;
    WebView::Application::settings().set_appearance(appearance);
    send_view_preferences();
}

void EngineServer::handle_set_preferred_contrast(JsonObject const& command)
{
    auto value = command.get_string("value"sv);
    if (!value.has_value())
        return;

    auto contrast = preferred_contrast_from_string(*value);
    if (!contrast.has_value())
        return;

    EngineApplication::the().set_preferred_contrast(*contrast);
    send_view_preferences();
}

void EngineServer::handle_set_preferred_motion(JsonObject const& command)
{
    auto value = command.get_string("value"sv);
    if (!value.has_value())
        return;

    auto motion = preferred_motion_from_string(*value);
    if (!motion.has_value())
        return;

    EngineApplication::the().set_preferred_motion(*motion);
    send_view_preferences();
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

void EngineServer::send_event(JsonObject event)
{
    static constexpr size_t max_ipc_buffer_bytes = 64 * MiB;
    auto serialized = event.serialized();
    auto bytes = serialized.bytes();
    if (m_write_buffer.size() - m_write_offset + bytes.size() + 1 > max_ipc_buffer_bytes) {
        warnln("LadybirdEngine: IPC write buffer limit exceeded");
        quit(1);
        return;
    }
    m_write_buffer.ensure_capacity(m_write_buffer.size() + bytes.size() + 1);
    m_write_buffer.append(bytes.data(), bytes.size());
    m_write_buffer.append('\n');
    flush_writes();
}

void EngineServer::send_event_with_fd(JsonObject event, int fd)
{
    static constexpr size_t max_ipc_buffer_bytes = 64 * MiB;
    auto serialized = event.serialized();
    auto bytes = serialized.bytes();
    if (m_write_buffer.size() - m_write_offset + bytes.size() + 1 > max_ipc_buffer_bytes) {
        warnln("LadybirdEngine: IPC write buffer limit exceeded");
        quit(1);
        return;
    }

    int dup_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        warnln("LadybirdEngine: could not duplicate frame buffer fd: {}", strerror(errno));
        return;
    }

    m_pending_write_fds.append({ m_write_buffer.size(), dup_fd });
    m_write_buffer.ensure_capacity(m_write_buffer.size() + bytes.size() + 1);
    m_write_buffer.append(bytes.data(), bytes.size());
    m_write_buffer.append('\n');
    flush_writes();
}

void EngineServer::flush_writes()
{
    while (m_write_offset < m_write_buffer.size()) {
        ErrorOr<size_t> result { 0 };
        bool sent_fd = false;

        if (!m_pending_write_fds.is_empty() && m_pending_write_fds.first().offset == m_write_offset) {
            auto chunk_end = m_pending_write_fds.size() > 1 ? m_pending_write_fds[1].offset : m_write_buffer.size();
            auto slice = m_write_buffer.span().slice(m_write_offset, chunk_end - m_write_offset);
            Vector<int, 1> fds;
            fds.append(m_pending_write_fds.first().fd);
            result = m_socket->send_message(slice, MSG_NOSIGNAL, move(fds));
            sent_fd = true;
        } else {
            auto chunk_end = !m_pending_write_fds.is_empty() ? m_pending_write_fds.first().offset : m_write_buffer.size();
            auto slice = m_write_buffer.span().slice(m_write_offset, chunk_end - m_write_offset);
            result = m_socket->write_some(slice);
        }

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
        if (sent_fd) {
            ::close(m_pending_write_fds.first().fd);
            m_pending_write_fds.remove(0);
        }
        m_write_offset += written;
        if (m_write_offset == m_write_buffer.size()) {
            m_write_buffer.clear_with_capacity();
            m_write_offset = 0;
        } else if (m_write_offset >= 64 * KiB && m_write_offset * 2 >= m_write_buffer.size()) {
            m_write_buffer.remove(0, m_write_offset);
            for (auto& pending : m_pending_write_fds)
                pending.offset -= m_write_offset;
            m_write_offset = 0;
        }
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
    event.set("bookmarked"sv, EngineApplication::the().bookmark_store().is_bookmarked(tab.view->url()));
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
        item.set("inline"sv, suggestion.can_be_inline_completed);
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
    event.set("can_go_back"sv, tab.view->can_navigate_back());
    event.set("can_go_forward"sv, tab.view->can_navigate_forward());
    send_event(move(event));
}

void EngineServer::send_dialog(Tab& tab, StringView kind, StringView message, StringView default_text)
{
    tab.pending_dialog_kind = String::from_utf8_with_replacement_character(kind);
    JsonObject event;
    event.set("type"sv, "dialog"sv);
    event.set("tab"sv, tab.id);
    event.set("kind"sv, kind);
    event.set("message"sv, message);
    event.set("default"sv, default_text);
    send_event(move(event));
}

static void append_bookmark_items(JsonArray& items, Vector<Ladybird::BookmarkItem> const& source)
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
    m_cached_autocomplete_bookmarks.clear();

    JsonArray items;
    append_bookmark_items(items, EngineApplication::the().bookmark_store().root_items());

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

static StringView context_menu_kind_to_string(WebView::ContextMenuKind kind)
{
    switch (kind) {
    case WebView::ContextMenuKind::Page:
        return "page"sv;
    case WebView::ContextMenuKind::Link:
        return "link"sv;
    case WebView::ContextMenuKind::Image:
        return "image"sv;
    case WebView::ContextMenuKind::Media:
        return "media"sv;
    }
    VERIFY_NOT_REACHED();
}

void EngineServer::handle_context_menu_request(u64 tab_id, WebView::ContextMenuRequest request)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    send_context_menu(*tab, move(request));
}

void EngineServer::send_context_menu(Tab& tab, WebView::ContextMenuRequest request)
{
    auto position = request.widget_position;
    tab.context_menu_actions.clear();

    JsonArray items;
    auto add = [&](String text, Function<void()> on_activate, bool enabled = true) {
        JsonObject entry;
        entry.set("text"sv, text);
        entry.set("action"sv, static_cast<u64>(tab.context_menu_actions.size()));
        entry.set("enabled"sv, enabled);
        items.must_append(move(entry));
        tab.context_menu_actions.append({ move(text), enabled, false, false, move(on_activate) });
    };
    auto add_checkable = [&](String text, bool checked, Function<void()> on_activate) {
        JsonObject entry;
        entry.set("text"sv, text);
        entry.set("action"sv, static_cast<u64>(tab.context_menu_actions.size()));
        entry.set("enabled"sv, true);
        entry.set("checkable"sv, true);
        entry.set("checked"sv, checked);
        items.must_append(move(entry));
        tab.context_menu_actions.append({ move(text), true, true, checked, move(on_activate) });
    };
    auto add_separator = [&] {
        JsonObject entry;
        entry.set("separator"sv, true);
        items.must_append(move(entry));
    };

    auto on_tab = [this, tab_id = tab.id](Function<void(Tab&)> callback) {
        return [this, tab_id, callback = move(callback)]() mutable {
            if (auto* tab = tab_for_id(tab_id))
                callback(*tab);
        };
    };
    auto copy_text = [](String const& text) {
        if (text.is_empty())
            return;
        EngineApplication::the().insert_clipboard_item({ { { "text/plain"_string, text.to_byte_string() } } });
    };

    auto& view = *tab.view;

    switch (request.kind) {
    case WebView::ContextMenuKind::Page:
        if (request.is_editable) {
            add("Undo"_string, on_tab([](Tab& tab) { tab.view->undo(); }));
            add("Redo"_string, on_tab([](Tab& tab) { tab.view->redo(); }));
            add_separator();
            add("Cut"_string, on_tab([this](Tab& tab) { handle_clipboard_copy(tab.id, true); }));
            add("Copy"_string, on_tab([this](Tab& tab) { handle_clipboard_copy(tab.id, false); }));
            add("Paste"_string, on_tab([](Tab& tab) { tab.view->paste_from_clipboard(Web::Clipboard::PasteMode::AllMimeTypes); }));
            add_separator();
            add("Select All"_string, on_tab([](Tab& tab) { tab.view->select_all(); }));
        } else {
            add("Back"_string, on_tab([](Tab& tab) { tab.view->traverse_the_history_by_delta(-1); }), view.can_navigate_back());
            add("Forward"_string, on_tab([](Tab& tab) { tab.view->traverse_the_history_by_delta(+1); }), view.can_navigate_forward());
            add("Reload"_string, on_tab([](Tab& tab) { tab.view->reload(); }));
            add_separator();
            if (request.selected_text.has_value())
                add("Copy"_string, on_tab([this](Tab& tab) { handle_clipboard_copy(tab.id, false); }));
            add("Paste"_string, on_tab([](Tab& tab) { tab.view->paste_from_clipboard(Web::Clipboard::PasteMode::AllMimeTypes); }));
            add("Select All"_string, on_tab([](Tab& tab) { tab.view->select_all(); }));
        }
        if (view.can_view_source()) {
            add_separator();
            add("View Page Source"_string, on_tab([](Tab& tab) { tab.view->get_source(); }));
        }
        if (request.can_inspect_element) {
            add_separator();
            add("Inspect Element"_string, on_tab([](Tab& tab) { tab.view->inspect_context_menu_element(); }));
        }
        break;

    case WebView::ContextMenuKind::Link:
        if (request.link_navigation.has_value()) {
            auto navigation = *request.link_navigation;
            add("Open Link"_string, on_tab([navigation](Tab& tab) { tab.view->load(navigation); }));
            add("Open Link in New Tab"_string, on_tab([navigation](Tab& tab) {
                tab.view->open_url_in_new_tab(navigation.url, Web::HTML::ActivateTab::No);
            }));
            add_separator();
            add("Copy Link Address"_string, [copy_text, url = navigation.url.to_string()] { copy_text(url); });
            add("Download Linked File"_string, on_tab([navigation](Tab& tab) {
                tab.view->download_url(navigation.url, WebView::ViewImplementation::PromptForPath::Yes);
            }));
        }
        if (request.can_inspect_element) {
            add_separator();
            add("Inspect Element"_string, on_tab([](Tab& tab) { tab.view->inspect_context_menu_element(); }));
        }
        break;

    case WebView::ContextMenuKind::Image:
        if (request.link_navigation.has_value()) {
            auto navigation = *request.link_navigation;
            add("Open Link"_string, on_tab([navigation](Tab& tab) { tab.view->load(navigation); }));
            add("Open Link in New Tab"_string, on_tab([navigation](Tab& tab) {
                tab.view->open_url_in_new_tab(navigation.url, Web::HTML::ActivateTab::No);
            }));
            add_separator();
        }
        if (request.navigation.has_value()) {
            auto navigation = *request.navigation;
            add("Open Image"_string, on_tab([navigation](Tab& tab) { tab.view->load(navigation); }));
            add("Open Image in New Tab"_string, on_tab([navigation](Tab& tab) {
                tab.view->open_url_in_new_tab(navigation.url, Web::HTML::ActivateTab::No);
            }));
            add_separator();
            add("Copy Image Address"_string, [copy_text, url = navigation.url.to_string()] { copy_text(url); });
            add("Save Image As…"_string, on_tab([navigation, mime = request.image_mime_type](Tab& tab) {
                tab.view->download_url(navigation.url, WebView::ViewImplementation::PromptForPath::Yes, mime);
            }));
        }
        add("Copy Image"_string, [bitmap = request.image_bitmap] {
            if (!bitmap.has_value() || !bitmap->is_valid())
                return;
            auto encoded = Gfx::PNGWriter::encode(*bitmap->bitmap());
            if (encoded.is_error())
                return;
            EngineApplication::the().insert_clipboard_item({ { { "image/png"_string, ByteString { encoded.value().bytes() } } } });
        }, request.image_bitmap.has_value() && request.image_bitmap->is_valid());
        if (request.can_inspect_element) {
            add_separator();
            add("Inspect Element"_string, on_tab([](Tab& tab) { tab.view->inspect_context_menu_element(); }));
        }
        break;

    case WebView::ContextMenuKind::Media:
        if (request.media.has_value()) {
            auto const& media = *request.media;
            auto send_media_action = [on_tab](WebView::ContextMenuMediaAction action) {
                return on_tab([action](Tab& tab) { tab.view->do_context_menu_media_action(action); });
            };

            add(media.is_playing ? "Pause"_string : "Play"_string, send_media_action(WebView::ContextMenuMediaAction::TogglePlay));
            add(media.is_muted ? "Unmute"_string : "Mute"_string, send_media_action(WebView::ContextMenuMediaAction::ToggleMute));
            add(media.has_user_agent_controls ? "Hide Controls"_string : "Show Controls"_string, send_media_action(WebView::ContextMenuMediaAction::ToggleControls));
            add_checkable("Loop"_string, media.is_looping, send_media_action(WebView::ContextMenuMediaAction::ToggleLoop));
            add_separator();
            if (media.is_picture_in_picture || media.can_enter_picture_in_picture)
                add(media.is_picture_in_picture ? "Exit Picture in Picture"_string : "Picture in Picture"_string, send_media_action(WebView::ContextMenuMediaAction::TogglePictureInPicture));
            if (media.is_video)
                add(media.is_fullscreen ? "Exit Full Screen"_string : "Full Screen"_string, send_media_action(WebView::ContextMenuMediaAction::ToggleFullscreen));
            if (media.media_url.has_value()) {
                add_separator();
                add(media.is_video ? "Open Video in New Tab"_string : "Open Audio in New Tab"_string, on_tab([url = *media.media_url](Tab& tab) {
                    tab.view->open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
                }));
                add(media.is_video ? "Copy Video Address"_string : "Copy Audio Address"_string, [copy_text, url = media.media_url->to_string()] { copy_text(url); });
            }
        }
        if (request.can_inspect_element) {
            add_separator();
            add("Inspect Element"_string, on_tab([](Tab& tab) { tab.view->inspect_context_menu_element(); }));
        }
        break;
    }

    JsonObject event;
    event.set("type"sv, "context_menu"sv);
    event.set("tab"sv, tab.id);
    event.set("kind"sv, context_menu_kind_to_string(request.kind));
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

    if (actions[*action_index].on_activate)
        actions[*action_index].on_activate();
}

void EngineServer::send_drag_start(u64 tab_id, Web::DragData drag_data)
{
    static constexpr size_t maximum_file_bytes = 8 * MiB;

    JsonArray items;
    for (auto const& item : drag_data.items) {
        JsonObject object;
        if (item.kind == Web::DragDataItem::Kind::Text) {
            object.set("kind"sv, "text"sv);
            object.set("type"sv, item.type);
            object.set("data"sv, item.text);
        } else {
            if (item.file_data.size() > maximum_file_bytes)
                continue;
            auto encoded = AK::encode_base64(item.file_data.bytes());
            if (encoded.is_error())
                continue;
            object.set("kind"sv, "file"sv);
            object.set("type"sv, item.type);
            object.set("name"sv, item.file_name);
            object.set("data"sv, encoded.release_value());
        }
        items.must_append(move(object));
    }

    JsonObject event;
    event.set("type"sv, "drag_start"sv);
    event.set("tab"sv, tab_id);
    event.set("items"sv, move(items));
    send_event(move(event));
}

void EngineServer::send_permission_request(Tab& tab, WebView::PermissionPromptRequest const& request, u64 request_id)
{
    JsonObject event;
    event.set("type"sv, "permission"sv);
    event.set("tab"sv, tab.id);
    event.set("request"sv, request_id);
    event.set("name"sv, Web::Permissions::to_string(request.name));
    event.set("origin"sv, request.top_level_origin.serialize());
    event.set("file"sv, request.is_file);
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

    auto pending = tab->pending_permissions.take(*request_id);
    if (pending.has_value()) {
        auto granted = command.get_bool("granted"sv).value_or(false);
        auto remember = command.get_bool("remember"sv).value_or(false);
        auto outcome = granted
            ? (remember ? Web::Permissions::PromptOutcome::AllowAlways : Web::Permissions::PromptOutcome::AllowOnce)
            : Web::Permissions::PromptOutcome::Block;
        pending->callback(outcome);
    }
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
        if (auto* store = EngineApplication::the().session_store(WebView::IsPrivate::No)) {
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
        EngineApplication::the().open_download(*download);
    } else if (action_name == "show_in_folder"sv) {
        EngineApplication::the().show_download_in_folder(*download);
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
