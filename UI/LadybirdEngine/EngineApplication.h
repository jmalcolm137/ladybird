/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>

namespace LadybirdSDE {

// The engine half of the LibGUI frontend. It is an ordinary Ladybird UI process
// (it owns the WebContent/RequestServer/... children), but it has no native
// chrome of its own: a LibGUI process connects over a Unix socket and drives it.
class EngineApplication final : public WebView::Application {
    WEB_VIEW_APPLICATION(EngineApplication)

public:
    ByteString const& engine_socket_path() const { return m_engine_socket_path; }
    ByteString const& frame_directory() const { return m_frame_directory; }

    using OpenBlankNewTabHandler = Function<Optional<WebView::ViewImplementation&>(Web::HTML::ActivateTab)>;
    void set_open_blank_new_tab_handler(OpenBlankNewTabHandler handler) { m_open_blank_new_tab_handler = move(handler); }

    using OpenNewWindowHandler = Function<void(URL::URL const&, WebView::IsPrivate)>;
    void set_open_new_window_handler(OpenNewWindowHandler handler) { m_open_new_window_handler = move(handler); }

    using ActiveWebViewProvider = Function<Optional<WebView::ViewImplementation&>()>;
    void set_active_web_view_provider(ActiveWebViewProvider provider) { m_active_web_view_provider = move(provider); }

protected:
    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual WebView::SessionLaunchPolicy launch_restore_policy() const override;
    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual void open_download(WebView::FileDownloader::Download const&) const override;
    virtual void show_download_in_folder(WebView::FileDownloader::Download const&) const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;
    virtual void open_url_in_new_window(URL::URL const&, WebView::IsPrivate) override;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, WebView::IsPrivate) override;

private:
    ByteString m_engine_socket_path;
    ByteString m_frame_directory;
    mutable OpenBlankNewTabHandler m_open_blank_new_tab_handler;
    mutable OpenNewWindowHandler m_open_new_window_handler;
    ActiveWebViewProvider m_active_web_view_provider;
};

}
