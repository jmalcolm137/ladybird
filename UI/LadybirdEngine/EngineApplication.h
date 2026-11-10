/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/FileDownloader.h>

namespace Ladybird {
class BookmarkStore;
class SessionStore;
}

namespace LadybirdSDE {

// The engine half of the LibGUI frontend. It is an ordinary Ladybird UI process
// (it owns the WebContent/RequestServer/... children), but it has no native
// chrome of its own: a LibGUI process connects over a Unix socket and drives it.
class EngineApplication final : public WebView::Application
    , public WebView::AutocompleteHost {
    WEB_VIEW_APPLICATION(EngineApplication)

public:
    EngineApplication();
    virtual ~EngineApplication();

    ByteString const& engine_socket_path() const { return m_engine_socket_path; }
    ByteString const& frame_directory() const { return m_frame_directory; }

    // Bookmarks and history are UI state the reference frontend keeps in its UI process. The engine host owns them here
    // because the LibGUI chrome is a separate process.
    Ladybird::BookmarkStore& bookmark_store();
    WebView::HistoryStore& history_store();
    Ladybird::SessionStore* session_store(WebView::IsPrivate = WebView::IsPrivate::No);
    void flush_session_store();

    void open_download(WebView::FileDownloader::Download const&) const;
    void show_download_in_folder(WebView::FileDownloader::Download const&) const;

    using OpenBlankNewTabHandler = Function<Optional<WebView::ViewImplementation&>(Web::HTML::ActivateTab)>;
    void set_open_blank_new_tab_handler(OpenBlankNewTabHandler handler) { m_open_blank_new_tab_handler = move(handler); }

    using OpenNewWindowHandler = Function<void(URL::URL const&, WebView::IsPrivate)>;
    void set_open_new_window_handler(OpenNewWindowHandler handler) { m_open_new_window_handler = move(handler); }

    using ActiveWebViewProvider = Function<Optional<WebView::ViewImplementation&>()>;
    void set_active_web_view_provider(ActiveWebViewProvider provider) { m_active_web_view_provider = move(provider); }

    // The clipboard and the DevTools client window belong to the chrome process. These handlers carry the engine's
    // side of that conversation over the control socket.
    using ClipboardWriteHandler = Function<void(Web::Clipboard::SystemClipboardItem const&)>;
    void set_clipboard_write_handler(ClipboardWriteHandler handler) { m_clipboard_write_handler = move(handler); }
    void cache_clipboard_item_from_frontend(Web::Clipboard::SystemClipboardItem);
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem) override;

    using DevToolsEventHandler = Function<void(bool enabled, u16 port)>;
    void set_devtools_event_handler(DevToolsEventHandler handler) { m_devtools_event_handler = move(handler); }
    Optional<u16> devtools_port_if_enabled() const;

protected:
    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;
    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab, WebView::ViewImplementation const* = nullptr) const override;
    virtual void open_url_in_new_window(URL::URL const&, WebView::IsPrivate) override;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, WebView::IsPrivate) override;

private:
    WebView::AutocompleteService& autocomplete_service() override;
    WebView::Settings& autocomplete_settings() override { return WebView::Application::settings(); }
    void resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback) const override;
    Function<void()> fetch_remote_suggestions(WebView::SearchEngine const&, String const& query, WebView::IsPrivate, Function<void(WebView::AutocompleteHost::RemoteResponse)> on_response) override;

    ByteString m_engine_socket_path;
    ByteString m_frame_directory;
    OwnPtr<WebView::AutocompleteService> m_autocomplete_service;
    OwnPtr<WebView::FaviconStore> m_favicon_store;
    OwnPtr<Ladybird::BookmarkStore> m_bookmark_store;
    OwnPtr<WebView::HistoryStore> m_history_store;
    OwnPtr<Ladybird::SessionStore> m_session_store;
    OwnPtr<Ladybird::SessionStore> m_private_session_store;
    mutable OpenBlankNewTabHandler m_open_blank_new_tab_handler;
    mutable OpenNewWindowHandler m_open_new_window_handler;
    ActiveWebViewProvider m_active_web_view_provider;
    ClipboardWriteHandler m_clipboard_write_handler;
    DevToolsEventHandler m_devtools_event_handler;
    mutable bool m_devtools_enabled { false };
};

}
