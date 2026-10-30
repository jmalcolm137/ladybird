/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserSession.h"

#include "BrowserWindow.h"
#include "ClipboardBridge.h"

#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <AK/StringBuilder.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>

namespace LadybirdSDE {

static Optional<ByteString> find_engine_path()
{
    if (auto configured = Core::Environment::get("LADYBIRD_ENGINE_HOST"sv); configured.has_value() && !configured->is_empty())
        return configured->to_byte_string();

    auto executable = Core::System::current_executable_path();
    if (executable.is_error())
        return {};

    auto directory = LexicalPath::dirname(executable.value());
    Vector<ByteString> candidates;
    candidates.append(LexicalPath::join(directory, "LadybirdEngine"sv).string());
    candidates.append(LexicalPath::join(directory, ".."sv, "libexec"sv, "LadybirdEngine"sv).string());
    candidates.append(LexicalPath::join(directory, ".."sv, ".."sv, "Build"sv, "release"sv, "libexec"sv, "LadybirdEngine"sv).string());
    candidates.append("/usr/lib/sde-next/libexec/LadybirdEngine"sv);
    candidates.append("/usr/lib/sde/libexec/LadybirdEngine"sv);
    candidates.append("/usr/local/libexec/LadybirdEngine"sv);

    for (auto const& candidate : candidates) {
        auto file = Core::File::open(candidate, Core::File::OpenMode::Read);
        if (!file.is_error())
            return candidate;
    }

    return {};
}

static Vector<String> urls_from_tabs(JsonArray const& tabs)
{
    Vector<String> urls;
    for (auto const& value : tabs.values()) {
        if (!value.is_object())
            continue;
        auto url = value.as_object().get_byte_string("url"sv);
        if (url.has_value() && !url->is_empty())
            urls.append(String::from_byte_string(url.value()).release_value());
    }
    return urls;
}

static Optional<Gfx::IntSize> size_from_object(JsonObject const& object)
{
    auto width = object.get_i64("width"sv);
    auto height = object.get_i64("height"sv);
    if (!width.has_value() || !height.has_value() || *width <= 0 || *height <= 0)
        return {};
    return Gfx::IntSize { static_cast<int>(*width), static_cast<int>(*height) };
}

static Optional<Gfx::IntPoint> position_from_object(JsonObject const& object)
{
    auto x = object.get_i64("x"sv);
    auto y = object.get_i64("y"sv);
    if (!x.has_value() || !y.has_value())
        return {};
    return Gfx::IntPoint { static_cast<int>(*x), static_cast<int>(*y) };
}

ErrorOr<NonnullOwnPtr<BrowserSession>> BrowserSession::create(Vector<String> initial_urls, bool private_window, Optional<u16> devtools_port)
{
    auto engine_path = find_engine_path();
    if (!engine_path.has_value())
        return Error::from_string_literal("LadybirdEngine was not found next to the browser or in libexec");

    auto session = adopt_own(*new BrowserSession());
    session->m_engine = TRY(EngineClient::launch(*engine_path, devtools_port));
    session->m_clipboard = make<ClipboardBridge>(*session->m_engine);

    auto* session_pointer = session.ptr();
    session->m_engine->on_connected = [session_pointer] {
        session_pointer->on_engine_connected();
    };
    session->m_engine->on_event = [session_pointer](JsonObject const& event) {
        session_pointer->on_engine_event(event);
    };
    session->m_engine->on_frame = [session_pointer](u64 tab_id, Gfx::Bitmap const* frame, u64 sequence, Gfx::IntRect damage, bool full) {
        session_pointer->on_engine_frame(tab_id, frame, sequence, damage, full);
    };
    session->m_engine->on_disconnected = [session_pointer] {
        session_pointer->on_engine_disconnected();
    };

    session->m_clipboard->start();
    TRY(session->initialize(move(initial_urls), private_window));
    return session;
}

BrowserSession::~BrowserSession()
{
    if (m_autosave_timer)
        m_autosave_timer->stop();
    save_session();

    // Destroying the engine client tears down the frame buffers, and that reports a cleared frame for every tab.
    // The windows are already gone by then, so stop the callbacks before the members are destroyed.
    if (m_engine) {
        m_engine->on_frame = {};
        m_engine->on_event = {};
        m_engine->on_connected = {};
        m_engine->on_disconnected = {};
    }
}

ErrorOr<void> BrowserSession::initialize(Vector<String> initial_urls, bool private_window)
{
    if (!private_window)
        load_permissions();

    if (!private_window && should_restore_session() && !Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv)) {
        // The engine host owns the native session store and sends its launch plan once connected. Wait for it so
        // restored tabs and their history are registered under the same session IDs. URLs passed on the command
        // line open in the restored window, as in the reference frontend.
        m_pending_initial_urls = move(initial_urls);
        m_waiting_for_session_plan = true;
        m_session_plan_timeout = Core::Timer::create_single_shot(2000, [this] {
            if (m_session_plan_received)
                return;
            m_session_plan_received = true;
            m_waiting_for_session_plan = false;
            if (m_windows.is_empty())
                create_window_internal(move(m_pending_initial_urls), false);
        });
        m_session_plan_timeout->start();
    } else {
        if (!create_window_internal(move(initial_urls), private_window))
            return Error::from_string_literal("Unable to create the browser window");
    }

    m_autosave_timer = Core::Timer::create_repeating(30 * 1000, [this] {
        save_session();
    });
    m_autosave_timer->start();
    return {};
}

void BrowserSession::handle_session_plan(JsonObject const& event)
{
    if (m_session_plan_received)
        return;

    m_session_plan_received = true;
    m_waiting_for_session_plan = false;
    if (m_session_plan_timeout)
        m_session_plan_timeout->stop();

    auto windows = event.get_array("windows"sv);
    if (!windows.has_value() || windows->is_empty()) {
        // Migration path for sessions written before the engine host used the
        // native SessionStore. Once those tabs are open they are registered
        // with the native store, and later launches use session_plan. Do not
        // resurrect an old URL-only session when the user passed URLs.
        if (m_windows.is_empty() && m_pending_initial_urls.is_empty() && load_session())
            return;
        if (m_windows.is_empty())
            create_window_internal(move(m_pending_initial_urls), false);
        return;
    }

    for (auto const& value : windows->values()) {
        if (!value.is_object())
            continue;
        auto const& object = value.as_object();

        Vector<InitialTab> tabs;
        if (auto tab_values = object.get_array("tabs"sv); tab_values.has_value()) {
            for (auto const& tab_value : tab_values->values()) {
                if (!tab_value.is_object())
                    continue;
                auto const& tab_object = tab_value.as_object();
                InitialTab tab;
                tab.url = String::from_byte_string(tab_object.get_byte_string("url"sv).value_or({})).release_value();
                tab.session_tab_id = tab_object.get_u64("session_tab"sv).value_or(0);
                tab.pinned = tab_object.get_bool("pinned"sv).value_or(false);
                tabs.append(move(tab));
            }
        }

        auto session_window_id = object.get_u64("window"sv).value_or(0);
        auto window = make<BrowserWindow>(*this, *m_engine, m_next_window_id++, false, session_window_id);
        auto active_tab = object.get_u64("active"sv);
        if (auto result = window->initialize(move(tabs), active_tab.has_value() ? Optional<size_t> { static_cast<size_t>(*active_tab) } : Optional<size_t> {}, true); result.is_error()) {
            warnln("Ladybird: could not restore a browser window: {}", result.error());
            continue;
        }

        auto* window_pointer = window.ptr();
        m_windows.append(move(window));

        if (auto width = object.get_i64("width"sv); width.has_value()) {
            if (auto height = object.get_i64("height"sv); height.has_value() && *width > 0 && *height > 0)
                window_pointer->set_window_size({ static_cast<int>(*width), static_cast<int>(*height) });
        }
        if (auto x = object.get_i64("x"sv); x.has_value()) {
            if (auto y = object.get_i64("y"sv); y.has_value())
                window_pointer->set_window_position({ static_cast<int>(*x), static_cast<int>(*y) });
        }
        window_pointer->show();
        if (object.get_bool("maximized"sv).value_or(false))
            window_pointer->set_maximized(true);
    }

    if (m_windows.is_empty()) {
        create_window_internal(move(m_pending_initial_urls), false);
        return;
    }

    if (!m_pending_initial_urls.is_empty() && !m_windows.is_empty())
        m_windows.last()->open_startup_urls(m_pending_initial_urls);
}

void BrowserSession::show()
{
    for (auto& window : m_windows)
        window->show();
}

void BrowserSession::quit()
{
    save_session();
    if (m_engine)
        m_engine->quit();
    GUI::Application::the()->quit(0);
}

BrowserWindow* BrowserSession::create_window(Vector<String> urls, bool private_window)
{
    return create_window_internal(move(urls), private_window);
}

void BrowserSession::note_closed_tab(String const& url, String const& title, bool private_tab)
{
    // A private tab must not become visible in a normal window later.
    if (private_tab || url.is_empty())
        return;

    m_closed_tabs.append({ url, title });
    if (m_closed_tabs.size() > 20)
        m_closed_tabs.remove(0);
}

bool BrowserSession::reopen_closed_tab()
{
    if (m_closed_tabs.is_empty())
        return false;

    auto closed = m_closed_tabs.take_last();
    auto* window = active_window();
    if (!window)
        window = create_window({}, false);
    if (!window)
        return false;

    window->reopen_tab(closed.url);
    return true;
}

BrowserWindow* BrowserSession::create_window_internal(Vector<String> urls, bool private_window, Optional<Gfx::IntSize> size, Optional<Gfx::IntPoint> position, Optional<size_t> active_tab, bool maximized, bool lazy_load)
{
    auto window = make<BrowserWindow>(*this, *m_engine, m_next_window_id++, private_window);
    if (auto result = window->initialize(move(urls), active_tab, lazy_load); result.is_error()) {
        warnln("Ladybird: could not create a browser window: {}", result.error());
        return nullptr;
    }

    auto* window_pointer = window.ptr();
    m_windows.append(move(window));

    if (position.has_value())
        window_pointer->set_window_position(*position);
    if (size.has_value())
        window_pointer->set_window_size(*size);
    if (active_tab.has_value())
        window_pointer->activate_tab_index(*active_tab);

    window_pointer->show();
    if (maximized)
        window_pointer->set_maximized(true);
    return window_pointer;
}

BrowserWindow* BrowserSession::active_window() const
{
    if (m_windows.is_empty())
        return nullptr;
    return m_windows.last().ptr();
}

BrowserWindow* BrowserSession::window_for_tab(u64 tab_id)
{
    for (auto& window : m_windows) {
        if (window->has_tab(tab_id))
            return window.ptr();
    }
    return nullptr;
}

BrowserWindow* BrowserSession::window_for_id(u64 window_id)
{
    for (auto& window : m_windows) {
        if (window->id() == window_id)
            return window.ptr();
    }
    return nullptr;
}

BrowserWindow* BrowserSession::window_for_request(u64 request_id)
{
    for (auto& window : m_windows) {
        if (window->has_request(request_id))
            return window.ptr();
    }
    return nullptr;
}

void BrowserSession::on_engine_connected()
{
    for (auto& window : m_windows)
        window->handle_engine_connected();
    m_clipboard->send_current_clipboard();
}

void BrowserSession::toggle_devtools()
{
    m_engine->toggle_devtools();
}

void BrowserSession::open_devtools_client()
{
    m_engine->open_devtools_client();
}

JsonArray BrowserSession::clipboard_items_for_paste()
{
    return m_clipboard->items_for_paste();
}

static String permission_key(String const& origin)
{
    auto view = origin.bytes_as_string_view();
    if (view.ends_with('/'))
        view = view.substring_view(0, view.length() - 1);
    return String::from_utf8(view).release_value();
}

void BrowserSession::note_permission(String const& origin, String const& name, bool granted, bool remember)
{
    if (origin.is_empty())
        return;
    auto& entries = m_permissions.ensure(permission_key(origin));
    for (auto& entry : entries) {
        if (entry.name == name) {
            entry.granted = granted;
            entry.persisted |= remember;
            if (remember)
                save_permissions();
            return;
        }
    }
    entries.append({ name, granted, remember });
    if (remember)
        save_permissions();
}

bool BrowserSession::remembered_permission(String const& origin, String const& name, bool& granted) const
{
    auto it = m_permissions.find(permission_key(origin));
    if (it == m_permissions.end())
        return false;
    for (auto const& entry : it->value) {
        if (entry.name == name && entry.persisted) {
            granted = entry.granted;
            return true;
        }
    }
    return false;
}

String BrowserSession::permission_summary(String const& origin) const
{
    auto it = m_permissions.find(permission_key(origin));
    if (it == m_permissions.end())
        return {};
    StringBuilder builder;
    for (size_t i = 0; i < it->value.size(); ++i) {
        if (i)
            builder.append(", "sv);
        builder.appendff("{} ({})", it->value[i].name, it->value[i].granted ? "allowed" : "blocked");
    }
    return builder.to_string().release_value();
}

void BrowserSession::forget_permissions(String const& origin)
{
    if (m_permissions.remove(permission_key(origin)))
        save_permissions();
}

ByteString BrowserSession::permissions_path() const
{
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    auto profile_name = profile.has_value() ? profile->to_byte_string() : ByteString { "libgui" };
    return LexicalPath::join(Core::StandardPaths::config_directory(), "ladybird-sde"sv, ByteString::formatted("permissions-{}.json", profile_name)).string();
}

void BrowserSession::load_permissions()
{
    auto file = Core::File::open(permissions_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return;

    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return;
    auto buffer = contents.release_value();
    StringView json { reinterpret_cast<char const*>(buffer.data()), buffer.size() };
    auto parsed = JsonParser { json }.parse();
    if (parsed.is_error() || !parsed.value().is_object())
        return;

    auto origins_value = parsed.value().as_object().get_object("origins"sv);
    if (!origins_value.has_value())
        return;

    origins_value->for_each_member([&](auto const& origin, auto const& entries_value) {
        if (!entries_value.is_array())
            return;
        auto& entries = m_permissions.ensure(String::from_byte_string(origin).release_value());
        for (auto const& entry_value : entries_value.as_array().values()) {
            if (!entry_value.is_object())
                continue;
            auto const& object = entry_value.as_object();
            auto name = String::from_byte_string(object.get_byte_string("name"sv).value_or({})).release_value();
            if (name.is_empty())
                continue;
            entries.append({ name, object.get_bool("granted"sv).value_or(false), true });
        }
    });
}

void BrowserSession::save_permissions() const
{
    JsonObject origins;
    for (auto const& [origin, entries] : m_permissions) {
        JsonArray persisted_entries;
        for (auto const& entry : entries) {
            if (!entry.persisted)
                continue;
            JsonObject object;
            object.set("name"sv, entry.name.bytes_as_string_view());
            object.set("granted"sv, entry.granted);
            persisted_entries.must_append(move(object));
        }
        if (!persisted_entries.is_empty())
            origins.set(origin.bytes_as_string_view(), move(persisted_entries));
    }

    auto path = permissions_path();
    if (auto result = Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes); result.is_error())
        return;

    JsonObject root;
    root.set("version"sv, 1);
    root.set("origins"sv, move(origins));

    auto temporary_path = ByteString::formatted("{}.tmp", path);
    auto file = Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return;
    auto serialized = root.to_byte_string();
    if (file.value()->write_until_depleted(serialized.bytes()).is_error())
        return;
    file.value()->close();
    if (Core::System::rename(temporary_path, path).is_error())
        (void)Core::System::unlink(temporary_path);
}

void BrowserSession::on_engine_event(JsonObject const& event)
{
    auto type = event.get_byte_string("type"sv).value_or({});

    if (type == "session_plan"sv) {
        handle_session_plan(event);
        return;
    }

    if (type == "clipboard_write"sv) {
        m_clipboard->apply_engine_clipboard(event);
        return;
    }

    if (type == "devtools_enabled"sv) {
        m_devtools_port = static_cast<u16>(event.get_u64("port"sv).value_or(0));
        for (auto& window : m_windows)
            window->set_devtools_state(m_devtools_port);
        return;
    }

    if (type == "devtools_disabled"sv) {
        m_devtools_port.clear();
        for (auto& window : m_windows)
            window->set_devtools_state({});
        return;
    }

    if (type == "devtools_error"sv) {
        auto message = String::from_byte_string(event.get_byte_string("message"sv).value_or({})).release_value();
        if (auto* window = active_window())
            window->show_engine_error(message);
        else
            warnln("Ladybird: {}", message);
        return;
    }

    if (type == "session_tab_registered"sv) {
        auto window_id = event.get_u64("window"sv);
        auto request_id = event.get_u64("request"sv);
        auto session_tab_id = event.get_u64("session_tab"sv);
        if (window_id.has_value() && request_id.has_value() && session_tab_id.has_value()) {
            if (auto* window = window_for_id(*window_id))
                window->handle_session_tab_registered(*request_id, *session_tab_id);
        }
        return;
    }

    if (type == "session_window"sv) {
        auto window_id = event.get_u64("window"sv);
        auto session_window_id = event.get_u64("session_window"sv);
        if (window_id.has_value() && session_window_id.has_value()) {
            if (auto* window = window_for_id(*window_id))
                window->set_session_window_id(*session_window_id);
        }
        return;
    }

    if (type == "tab_created"sv) {
        auto tab_id = event.get_u64("tab"sv);
        if (!tab_id.has_value())
            return;

        auto request_id = event.get_u64("request"sv).value_or(0);
        BrowserWindow* target = nullptr;
        if (request_id != 0) {
            target = window_for_request(request_id);
        } else if (auto parent = event.get_u64("parent"sv); parent.has_value()) {
            target = window_for_tab(*parent);
        }
        if (!target) {
            if (auto window_id = event.get_u64("window"sv); window_id.has_value())
                target = window_for_id(*window_id);
        }
        if (!target)
            target = active_window();
        if (!target)
            return;

        target->adopt_engine_tab(*tab_id, request_id);
        return;
    }

    if (type == "open_window"sv) {
        handle_open_window(event);
        return;
    }

    if (type == "history_recent"sv || type == "history_search_results"sv || type == "bookmarks"sv || type == "downloads"sv || type == "download_added"sv || type == "download_updated"sv || type == "download_removed"sv) {
        for (auto& window : m_windows)
            window->handle_engine_global_event(event);
        return;
    }

    auto tab_id = event.get_u64("tab"sv);
    if (!tab_id.has_value())
        return;

    if (auto* window = window_for_tab(*tab_id))
        window->handle_engine_tab_event(event, *tab_id);
}

void BrowserSession::on_engine_frame(u64 tab_id, Gfx::Bitmap const* frame, u64 sequence, Gfx::IntRect damage, bool full)
{
    if (auto* window = window_for_tab(tab_id))
        window->handle_engine_frame(tab_id, frame, sequence, damage, full);
}

void BrowserSession::on_engine_disconnected()
{
    for (auto& window : m_windows)
        window->handle_engine_disconnected();
}

void BrowserSession::handle_open_window(JsonObject const& event)
{
    auto url = event.get_byte_string("url"sv).value_or({});
    auto is_private = event.get_bool("private"sv).value_or(false);

    Vector<String> urls;
    if (!url.is_empty())
        urls.append(String::from_byte_string(url).release_value());
    create_window(move(urls), is_private);
}

void BrowserSession::window_will_close(BrowserWindow& window)
{
    window.close_engine_tabs();

    Core::deferred_invoke([this, &window] {
        auto window_count = m_windows.size();
        m_windows.remove_first_matching([&](auto const& candidate) {
            return candidate.ptr() == &window;
        });
        if (m_windows.size() == window_count)
            return;

        save_session();

        if (m_windows.is_empty()) {
            if (m_engine)
                m_engine->quit();
            GUI::Application::the()->quit(0);
        }
    });
}

ByteString BrowserSession::session_path() const
{
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    auto profile_name = profile.has_value() ? profile->to_byte_string() : ByteString { "libgui" };
    return LexicalPath::join(Core::StandardPaths::config_directory(), "ladybird-sde"sv, ByteString::formatted("session-{}.json", profile_name)).string();
}

bool BrowserSession::should_restore_session() const
{
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    auto profile_name = profile.has_value() ? profile->to_byte_string() : ByteString { "libgui" };
    auto settings_path = LexicalPath::join(Core::StandardPaths::config_directory(), "Ladybird"sv, "Profiles"sv, profile_name, "Settings.json"sv).string();
    auto file = Core::File::open(settings_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return false;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return false;
    auto buffer = contents.release_value();
    StringView json { reinterpret_cast<char const*>(buffer.data()), buffer.size() };
    auto parsed = JsonParser { json }.parse();
    if (parsed.is_error() || !parsed.value().is_object())
        return false;
    return parsed.value().as_object().get_bool("restorePreviousSession"sv).value_or(false);
}

void BrowserSession::save_session()
{
    if (Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv))
        return;

    for (auto& window : m_windows) {
        if (window->session_window_id() != 0) {
            m_engine->window_geometry(
                window->session_window_id(),
                window->window_position(),
                window->window_size(),
                window->is_maximized());
        }
    }

    JsonArray windows;
    for (auto& window : m_windows) {
        if (window->is_private())
            continue;

        JsonArray tabs;
        for (auto const& url : window->tab_urls()) {
            JsonObject entry;
            entry.set("url"sv, url.is_empty() ? StringView { "about:newtab"sv } : url.bytes_as_string_view());
            tabs.must_append(move(entry));
        }
        if (tabs.is_empty())
            continue;

        auto size = window->window_size();
        auto position = window->window_position();
        JsonObject entry;
        entry.set("x"sv, position.x());
        entry.set("y"sv, position.y());
        entry.set("width"sv, size.width());
        entry.set("height"sv, size.height());
        entry.set("maximized"sv, window->is_maximized());
        entry.set("active"sv, static_cast<u64>(window->active_tab_index()));
        entry.set("tabs"sv, move(tabs));
        windows.must_append(move(entry));
    }

    if (windows.is_empty())
        return;

    auto path = session_path();
    if (auto result = Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes); result.is_error())
        return;

    JsonObject session;
    session.set("windows"sv, move(windows));

    auto temporary_path = ByteString::formatted("{}.tmp", path);
    auto file = Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return;

    auto serialized = session.to_byte_string();
    if (file.value()->write_until_depleted(serialized.bytes()).is_error())
        return;
    file.value()->close();
    if (Core::System::rename(temporary_path, path).is_error())
        (void)Core::System::unlink(temporary_path);
}

bool BrowserSession::load_session()
{
    if (Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv))
        return false;

    auto file = Core::File::open(session_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return false;

    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return false;

    auto buffer = contents.release_value();
    StringView json { reinterpret_cast<char const*>(buffer.data()), buffer.size() };
    auto parsed = JsonParser { json }.parse();
    if (parsed.is_error() || !parsed.value().is_object())
        return false;

    auto const& session = parsed.value().as_object();

    auto windows = session.get_array("windows"sv);
    if (!windows.has_value())
        return load_legacy_session(session);

    for (auto const& value : windows->values()) {
        if (!value.is_object())
            continue;
        auto const& entry = value.as_object();
        auto tabs = entry.get_array("tabs"sv);
        if (!tabs.has_value())
            continue;

        auto urls = urls_from_tabs(*tabs);
        if (urls.is_empty())
            continue;

        Optional<size_t> active_tab;
        if (auto active = entry.get_u64("active"sv); active.has_value())
            active_tab = static_cast<size_t>(*active);

        create_window_internal(move(urls), false, size_from_object(entry), position_from_object(entry), active_tab, entry.get_bool("maximized"sv).value_or(false), true);
    }

    return !m_windows.is_empty();
}

bool BrowserSession::load_legacy_session(JsonObject const& session)
{
    auto tabs = session.get_array("tabs"sv);
    if (!tabs.has_value())
        return false;

    auto urls = urls_from_tabs(*tabs);
    if (urls.is_empty())
        return false;

    Optional<size_t> active_tab;
    if (auto active = session.get_u64("active"sv); active.has_value())
        active_tab = static_cast<size_t>(*active);

    return create_window_internal(move(urls), false, size_from_object(session), position_from_object(session), active_tab, session.get_bool("maximized"sv).value_or(false), true) != nullptr;
}

}
