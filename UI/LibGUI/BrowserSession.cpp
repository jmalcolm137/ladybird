/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserSession.h"

#include "BrowserWindow.h"

#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
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

ErrorOr<NonnullOwnPtr<BrowserSession>> BrowserSession::create(Vector<String> initial_urls, bool private_window)
{
    auto engine_path = find_engine_path();
    if (!engine_path.has_value())
        return Error::from_string_literal("LadybirdEngine was not found next to the browser or in libexec");

    auto session = adopt_own(*new BrowserSession());
    session->m_engine = TRY(EngineClient::launch(*engine_path));

    auto* session_pointer = session.ptr();
    session->m_engine->on_connected = [session_pointer] {
        session_pointer->on_engine_connected();
    };
    session->m_engine->on_event = [session_pointer](JsonObject const& event) {
        session_pointer->on_engine_event(event);
    };
    session->m_engine->on_frame = [session_pointer](u64 tab_id, Gfx::Bitmap const* frame, u64 sequence, u64 generation, Gfx::IntRect damage, bool full) {
        session_pointer->on_engine_frame(tab_id, frame, sequence, generation, damage, full);
    };
    session->m_engine->on_disconnected = [session_pointer] {
        session_pointer->on_engine_disconnected();
    };

    TRY(session->initialize(move(initial_urls), private_window));
    return session;
}

BrowserSession::~BrowserSession()
{
    save_session();
}

ErrorOr<void> BrowserSession::initialize(Vector<String> initial_urls, bool private_window)
{
    if (initial_urls.is_empty() && !private_window && load_session())
        return {};

    if (!create_window_internal(move(initial_urls), private_window))
        return Error::from_string_literal("Unable to create the browser window");
    return {};
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

BrowserWindow* BrowserSession::create_window_internal(Vector<String> urls, bool private_window, Optional<Gfx::IntSize> size, Optional<size_t> active_tab)
{
    auto window = make<BrowserWindow>(*this, *m_engine, private_window);
    if (auto result = window->initialize(move(urls)); result.is_error()) {
        warnln("Ladybird: could not create a browser window: {}", result.error());
        return nullptr;
    }

    auto* window_pointer = window.ptr();
    m_windows.append(move(window));

    if (size.has_value())
        window_pointer->set_window_size(*size);
    if (active_tab.has_value())
        window_pointer->activate_tab_index(*active_tab);

    window_pointer->show();
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
}

void BrowserSession::on_engine_event(JsonObject const& event)
{
    auto type = event.get_byte_string("type"sv).value_or({});

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

    if (type == "bookmarks"sv || type == "downloads"sv || type == "download_added"sv || type == "download_updated"sv || type == "download_removed"sv) {
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

void BrowserSession::on_engine_frame(u64 tab_id, Gfx::Bitmap const* frame, u64 sequence, u64 generation, Gfx::IntRect damage, bool full)
{
    if (auto* window = window_for_tab(tab_id))
        window->handle_engine_frame(tab_id, frame, sequence, generation, damage, full);
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

void BrowserSession::restore_previous_session()
{
    if (load_session())
        show();
}

ByteString BrowserSession::session_path() const
{
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    auto profile_name = profile.has_value() ? profile->to_byte_string() : ByteString { "libgui" };
    return LexicalPath::join(Core::StandardPaths::config_directory(), "ladybird-sde"sv, ByteString::formatted("session-{}.json", profile_name)).string();
}

void BrowserSession::save_session()
{
    if (Core::Environment::has("LADYBIRD_SDE_DISABLE_SESSION_RESTORE"sv))
        return;

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
        JsonObject entry;
        entry.set("width"sv, size.width());
        entry.set("height"sv, size.height());
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

    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return;

    auto serialized = session.to_byte_string();
    if (file.value()->write_until_depleted(serialized.bytes()).is_error())
        return;
    file.value()->close();
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

        create_window_internal(move(urls), false, size_from_object(entry), active_tab);
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

    return create_window_internal(move(urls), false, size_from_object(session), active_tab) != nullptr;
}

}
