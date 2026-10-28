/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineServer.h"
#include "EngineApplication.h"

#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebCommon/HTML/WebViewHints.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/ViewImplementation.h>
#include <Shared/EngineProtocol.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace LadybirdSDE {

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

    JsonObject hello;
    hello.set("type"sv, "hello"sv);
    hello.set("protocol"sv, static_cast<unsigned>(protocol_version));
    send_event(move(hello));

    EngineApplication::the().set_open_blank_new_tab_handler([this](Web::HTML::ActivateTab) -> Optional<WebView::ViewImplementation&> {
        auto viewport_size = Gfx::IntSize { 1024, 768 };
        auto is_private = WebView::IsPrivate::No;
        if (auto* active = tab_for_id(m_active_tab_id)) {
            viewport_size = active->viewport_size;
            is_private = active->view->is_private();
        }
        auto& tab = create_tab(viewport_size, is_private == WebView::IsPrivate::Yes, 0);
        return static_cast<WebView::ViewImplementation&>(*tab.view);
    });

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
    } else {
        auto tab_id = command.get_u64("tab"sv);
        if (!tab_id.has_value()) {
            warnln("LadybirdEngine: '{}' command has no tab", *type);
            return;
        }

        if (*type == "activate"sv) {
            handle_activate_tab(*tab_id);
        } else if (*type == "navigate"sv) {
            handle_navigate(*tab_id, command);
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

EngineServer::Tab& EngineServer::create_tab(Gfx::IntSize viewport_size, bool is_private, u64 request_id)
{
    auto id = m_next_tab_id++;
    auto view = EngineHeadlessWebView::create(m_theme, viewport_size.to_type<Web::DevicePixels>(), is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);

    m_tabs.set(id, make<Tab>(id, request_id, move(view), viewport_size));
    auto& tab_ref = *m_tabs.find(id)->value;
    wire_tab(tab_ref);

    m_active_tab_id = id;
    send_tab_created(tab_ref);
    ensure_frame_buffer(tab_ref);
    send_frame(tab_ref);
    return tab_ref;
}

EngineServer::Tab& EngineServer::create_child_tab(Tab& parent, WebView::CanonicalTraversable& traversable)
{
    auto id = m_next_tab_id++;
    auto view = WebView::HeadlessWebView::create_child(*parent.view, traversable);

    m_tabs.set(id, make<Tab>(id, 0, move(view), parent.viewport_size));
    auto& tab_ref = *m_tabs.find(id)->value;
    wire_tab(tab_ref);

    send_tab_created(tab_ref);
    ensure_frame_buffer(tab_ref);
    send_frame(tab_ref);
    send_url(tab_ref);
    send_title(tab_ref);
    send_loading_state(tab_ref, tab_ref.view->is_loading());
    return tab_ref;
}

void EngineServer::wire_tab(Tab& tab)
{
    auto tab_id = tab.id;

    tab.view->on_ready_to_paint = [this, tab_id] {
        if (auto* tab = tab_for_id(tab_id))
            send_frame(*tab);
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
        }
    };

    tab.view->on_loading_state_change = [this, tab_id](bool loading) {
        if (auto* tab = tab_for_id(tab_id))
            send_loading_state(*tab, loading);
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

    if (m_active_tab_id == tab_id && !m_tabs.is_empty())
        m_active_tab_id = m_tabs.begin()->key;
}

void EngineServer::handle_create_tab(JsonObject const& command)
{
    auto width = command.get_i64("width"sv).value_or(1024);
    auto height = command.get_i64("height"sv).value_or(768);
    auto is_private = command.get_bool("private"sv).value_or(false);

    auto request_id = command.get_u64("request"sv).value_or(0);
    auto& tab = create_tab({ max(static_cast<int>(width), 1), max(static_cast<int>(height), 1) }, is_private, request_id);

    if (auto url = command.get_string("url"sv); url.has_value() && !url->is_empty())
        tab.view->load_from_user_input(url->bytes_as_string_view());
    else
        tab.view->load(URL::URL::about("newtab"sv));
}

void EngineServer::handle_activate_tab(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    m_active_tab_id = tab_id;
    for (auto& [id, other_tab] : m_tabs) {
        if (id == tab_id)
            other_tab->view->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
        else
            other_tab->view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
    }
}

void EngineServer::handle_navigate(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto url = command.get_string("url"sv);
    if (!url.has_value())
        return;

    if (command.get_bool("user_input"sv).value_or(true))
        tab->view->load_from_user_input(url->bytes_as_string_view());
    else if (auto parsed = URL::Parser::basic_parse(url->bytes_as_string_view()); parsed.has_value())
        tab->view->load(*parsed);
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

    Web::MouseEvent event;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)), static_cast<int>(command.get_i64("y"sv).value_or(0)) };
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

    Web::MouseEvent event;
    event.type = Web::MouseEvent::Type::MouseWheel;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)), static_cast<int>(command.get_i64("y"sv).value_or(0)) };
    event.screen_position = event.position;
    event.modifiers = modifiers_from_command(command);
    event.button = Web::UIEvents::MouseButton::None;
    event.buttons = Web::UIEvents::MouseButton::None;

    event.wheel_delta_x = command.get_double_with_precision_loss("delta_x"sv).value_or(0.0);
    event.wheel_delta_y = command.get_double_with_precision_loss("delta_y"sv).value_or(0.0);
    event.wheel_delta_precision = Web::WheelDeltaPrecision::Discrete;
    event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, event.wheel_delta_precision);
    event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, event.wheel_delta_precision);

    tab->view->enqueue_input_event(move(event));
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

    tab->view->enqueue_input_event(move(event));
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

    tab->view->find_in_page(Utf16String::from_utf8_with_replacement_character(query->bytes_as_string_view()));
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
    tab.frame_write_slot = 0;
}

bool EngineServer::ensure_frame_buffer(Tab& tab)
{
    auto width = tab.viewport_size.width();
    auto height = tab.viewport_size.height();
    if (width <= 0 || height <= 0)
        return false;

    auto slot_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (tab.frame_mapping && tab.frame_slot_size >= slot_size)
        return true;

    destroy_frame_buffer(tab);

    tab.frame_slot_size = slot_size;

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

    auto text = command.get_string("text"sv);
    if (!text.has_value())
        return;

    WebView::Application::the().insert_clipboard_entry({ "text/plain"_string, text->to_byte_string() });
    tab->view->paste_from_clipboard();
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

void EngineServer::send_frame(Tab& tab)
{
    auto bitmap = tab.view->displayed_bitmap();
    if (!bitmap)
        return;

    auto size = bitmap->size();
    if (size.is_empty())
        return;

    if (size != tab.viewport_size)
        tab.viewport_size = size;

    if (!ensure_frame_buffer(tab))
        return;

    auto slot = tab.frame_write_slot;
    tab.frame_write_slot = (tab.frame_write_slot + 1) % tab.frame_slots;
    auto* destination = static_cast<u8*>(tab.frame_mapping) + (static_cast<size_t>(slot) * tab.frame_slot_size);
    auto stride = static_cast<size_t>(size.width()) * 4;

    for (int y = 0; y < size.height(); ++y)
        memcpy(destination + (static_cast<size_t>(y) * stride), bitmap->scanline_u8(y), stride);

    auto sequence = ++tab.frame_sequence;

    JsonObject event;
    event.set("type"sv, "frame"sv);
    event.set("tab"sv, tab.id);
    event.set("slot"sv, slot);
    event.set("width"sv, size.width());
    event.set("height"sv, size.height());
    event.set("stride"sv, static_cast<u64>(stride));
    event.set("sequence"sv, sequence);
    send_event(move(event));
}

void EngineServer::send_event(JsonObject event)
{
    auto serialized = event.serialized();
    StringBuilder builder;
    builder.append(serialized.bytes_as_string_view());
    builder.append('\n');

    if (auto result = m_socket->write_until_depleted(builder.string_view().bytes()); result.is_error()) {
        warnln("LadybirdEngine: socket write failed: {}", result.error());
        quit();
    }
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

void EngineServer::send_bookmarks()
{
    JsonArray items;
    for (auto const& item : WebView::Application::bookmark_store().root_items()) {
        if (!item.is_bookmark())
            continue;

        auto const& bookmark = item.bookmark();
        JsonObject entry;
        auto title = bookmark.title.has_value() ? *bookmark.title : bookmark.url.to_string();
        entry.set("title"sv, title);
        entry.set("url"sv, bookmark.url.to_string());
        items.must_append(move(entry));
    }

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
    if (auto path = command.get_string("path"sv); path.has_value() && !path->is_empty()) {
        if (auto file = WebView::create_selected_file(path->to_byte_string()); file.is_error())
            warnln("LadybirdEngine: could not open selected file '{}': {}", path->bytes_as_string_view(), file.error());
        else
            selected_files.append(file.release_value());
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

    JsonObject event;
    event.set("type"sv, "select_dropdown"sv);
    event.set("tab"sv, tab.id);
    event.set("x"sv, position.x());
    event.set("y"sv, position.y());
    event.set("min_width"sv, minimum_width);
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
        (void)downloader.prune_inactive_downloads(WebView::IsPrivate::No);
        (void)downloader.prune_inactive_downloads(WebView::IsPrivate::Yes);
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
