/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineApplication.h"

#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/ImmutableBytes.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/SessionStore.h>

namespace LadybirdSDE {

EngineApplication::EngineApplication() = default;
EngineApplication::~EngineApplication() = default;

Ladybird::BookmarkStore& EngineApplication::bookmark_store()
{
    if (!m_bookmark_store) {
        auto path = LexicalPath::join(profile().paths().config, "Bookmarks.json"sv).string();
        m_bookmark_store = make<Ladybird::BookmarkStore>(Ladybird::BookmarkStore::create(move(path)));
    }
    return *m_bookmark_store;
}

WebView::HistoryStore& EngineApplication::history_store()
{
    if (!m_history_store) {
        auto database = Database::Database::create(profile().paths().data, "History"sv);
        if (!database.is_error()) {
            // History and favicons share the database; the history store's schema references the favicon tables.
            auto favicons = WebView::FaviconStore::migrate_schema(*database.value());
            auto history = WebView::HistoryStore::migrate_schema(*database.value());
            if (!favicons.is_error() && favicons.value() == Database::MigrationOutcome::Success
                && !history.is_error() && history.value() == Database::MigrationOutcome::Success) {
                if (auto favicon_store = WebView::FaviconStore::create(*database.value()); !favicon_store.is_error())
                    m_favicon_store = favicon_store.release_value();
                else
                    warnln("LadybirdEngine: could not open the favicon store: {}", favicon_store.error());
                auto store = WebView::HistoryStore::create(*database.value());
                if (!store.is_error())
                    m_history_store = store.release_value();
                else
                    warnln("LadybirdEngine: could not open the history store: {}", store.error());
            } else if (favicons.is_error()) {
                warnln("LadybirdEngine: could not migrate the favicon database: {}", favicons.error());
            } else if (history.is_error()) {
                warnln("LadybirdEngine: could not migrate the history database: {}", history.error());
            }
        } else {
            warnln("LadybirdEngine: could not open the history database: {}", database.error());
        }
        if (!m_history_store)
            m_history_store = WebView::HistoryStore::create_disabled();
    }
    return *m_history_store;
}

Ladybird::SessionStore* EngineApplication::session_store(WebView::IsPrivate is_private)
{
    auto& store = is_private == WebView::IsPrivate::Yes ? m_private_session_store : m_session_store;
    if (store)
        return store.ptr();

    if (is_private == WebView::IsPrivate::No) {
        auto policy = WebView::Application::settings().restore_previous_session()
            ? Ladybird::SessionLaunchPolicy::Everything
            : Ladybird::SessionLaunchPolicy::PinnedOnly;
        auto database = Database::Database::create(profile().paths().data, "Sessions"sv, { .foreign_keys = Database::Database::ForeignKeys::Yes });
        if (!database.is_error()) {
            auto migrated = Ladybird::SessionStore::migrate_schema(*database.value());
            if (!migrated.is_error() && migrated.value() == Database::MigrationOutcome::Success) {
                auto created = Ladybird::SessionStore::create(*database.value(), policy);
                if (!created.is_error())
                    store = created.release_value();
                else
                    warnln("LadybirdEngine: could not open the session store: {}", created.error());
            } else if (migrated.is_error()) {
                warnln("LadybirdEngine: could not migrate the sessions database: {}", migrated.error());
            }
        } else {
            warnln("LadybirdEngine: could not open the sessions database: {}", database.error());
        }
    }

    if (!store)
        store = Ladybird::SessionStore::create();
    return store.ptr();
}

void EngineApplication::flush_session_store()
{
    if (m_session_store) {
        m_session_store->flush_dirty_state();
        m_session_store->mark_clean_exit();
    }
    if (m_private_session_store)
        m_private_session_store->flush_dirty_state();
}

WebView::AutocompleteService& EngineApplication::autocomplete_service()
{
    if (!m_autocomplete_service) {
        // The service opens its own connection to the history database; migrate it before that connection exists.
        (void)history_store();
        m_autocomplete_service = make<WebView::AutocompleteService>(Core::EventLoop::current(), profile().paths().data);
    }
    return *m_autocomplete_service;
}

void EngineApplication::resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback callback) const
{
    callback(nullptr);
}

Function<void()> EngineApplication::fetch_remote_suggestions(WebView::SearchEngine const& engine, String const& query, WebView::IsPrivate is_private, Function<void(WebView::AutocompleteHost::RemoteResponse)> on_response)
{
    if (!engine.suggestions.has_value())
        return {};

    auto url = engine.suggestions_url_for_query(query);
    if (!url.has_value())
        return {};

    auto client = WebView::Application::request_server_client(is_private);
    if (client.is_error())
        return {};

    auto request = client.value()->start_request("GET"sv, *url, HTTP::Cookie::SameSiteContext::same_site());
    request->set_buffered_request_finished_callback(
        [on_response = move(on_response)](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, NonnullRefPtr<HTTP::HeaderList> headers, Optional<u32> response_code, Optional<String>, Requests::CacheState, Core::ImmutableBytes payload) mutable {
            WebView::AutocompleteHost::RemoteResponse response;
            response.had_network_error = network_error.has_value();
            response.response_code = response_code;
            for (auto const& header : headers->headers()) {
                if (header.name.equals_ignoring_ascii_case("Content-Type"sv)) {
                    response.content_type = header.value;
                    break;
                }
            }
            if (auto body = payload.copy_to_byte_buffer(); !body.is_error())
                response.body = body.release_value();
            on_response(move(response));
        });

    return [weak = request->make_weak_ptr()] {
        if (auto request = weak.strong_ref())
            request->stop();
    };
}

void EngineApplication::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(m_engine_socket_path, "Unix socket to accept the LibGUI chrome connection on", "engine-socket", 0, "path");
    args_parser.add_option(m_frame_directory, "Directory in which to publish frame files", "frame-directory", 0, "path");
}

static void open_with_system(ByteString const& path)
{
    Vector<ByteString> arguments;
    arguments.append(path);
    if (auto result = Core::Process::spawn("xdg-open"sv, arguments); result.is_error())
        warnln("LadybirdEngine: could not open '{}': {}", path, result.error());
}

Optional<WebView::ViewImplementation&> EngineApplication::open_blank_new_tab(Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const*) const
{
    if (!m_open_blank_new_tab_handler)
        return {};
    return m_open_blank_new_tab_handler(activate_tab);
}

void EngineApplication::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    if (m_open_new_window_handler) {
        m_open_new_window_handler(url, is_private);
        return;
    }

    warnln("LadybirdEngine: no frontend is connected to open a new window for '{}'", url);
}

void EngineApplication::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    open_url_in_new_window(navigation.url, is_private);
}

void EngineApplication::open_download(WebView::FileDownloader::Download const& download) const
{
    open_with_system(download.destination.string());
}

void EngineApplication::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    open_with_system(LexicalPath::dirname(download.destination.string()));
}

Optional<WebView::ViewImplementation&> EngineApplication::active_web_view() const
{
    if (!m_active_web_view_provider)
        return {};
    return m_active_web_view_provider();
}

void EngineApplication::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    // Keep the item so page-initiated reads (navigator.clipboard.read) and paste see the same data, then hand it to
    // the chrome process, which owns the real desktop clipboard.
    WebView::Application::insert_clipboard_item(item);
    if (m_clipboard_write_handler)
        m_clipboard_write_handler(item);
}

void EngineApplication::cache_clipboard_item_from_frontend(Web::Clipboard::SystemClipboardItem item)
{
    // Qualified call: the frontend is reporting the desktop clipboard, so do not echo it back as a page write.
    WebView::Application::insert_clipboard_item(move(item));
}

Optional<u16> EngineApplication::devtools_port_if_enabled() const
{
    if (!m_devtools_enabled)
        return {};
    return static_cast<u16>(browser_options().devtools_port.value_or(0));
}

void EngineApplication::on_devtools_enabled() const
{
    m_devtools_enabled = true;
    WebView::Application::on_devtools_enabled();
    if (m_devtools_event_handler)
        m_devtools_event_handler(true, browser_options().devtools_port.value_or(0));
}

void EngineApplication::on_devtools_disabled() const
{
    m_devtools_enabled = false;
    WebView::Application::on_devtools_disabled();
    if (m_devtools_event_handler)
        m_devtools_event_handler(false, 0);
}

void EngineApplication::create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    // The chrome is a separate process. Do not join an existing browser, and do
    // not create a window: this process only hosts pages. Do not set
    // headless_mode here: it would replace the persistent session store with a
    // transient one, losing native session-history restore.

    // Frames have to come back as CPU-visible shared bitmaps so the LibGUI
    // process can paint them without importing GPU handles across processes.
    web_content_options.force_cpu_painting = WebView::ForceCPUPainting::Yes;

    // A page that opens a link in a new tab should be able to ask the engine
    // host for one; the host turns that into a tab in the LibGUI window.
    browser_options.allow_popups = WebView::AllowPopups::Yes;
}

}
