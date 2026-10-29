/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineApplication.h"

#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/Process.h>

namespace LadybirdSDE {

void EngineApplication::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(m_engine_socket_path, "Unix socket to accept the LibGUI chrome connection on", "engine-socket", 0, "path");
    args_parser.add_option(m_frame_directory, "Directory in which to publish frame files", "frame-directory", 0, "path");
    args_parser.add_option(m_frame_socket_fd, "Inherited SOCK_SEQPACKET fd for zero-copy frame buffers", "frame-fd", 0, "fd");
}

static void open_with_system(ByteString const& path)
{
    Vector<ByteString> arguments;
    arguments.append(path);
    if (auto result = Core::Process::spawn("xdg-open"sv, arguments); result.is_error())
        warnln("LadybirdEngine: could not open '{}': {}", path, result.error());
}

Optional<WebView::ViewImplementation&> EngineApplication::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
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

void EngineApplication::create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    // The chrome is a separate process. Do not join an existing browser, and do
    // not create a window: this process only hosts pages.
    browser_options.headless_mode = WebView::HeadlessMode::Manual;

    // Frames have to come back as CPU-visible shared bitmaps so the LibGUI
    // process can paint them without importing GPU handles across processes.
    web_content_options.force_cpu_painting = WebView::ForceCPUPainting::Yes;

    // A page that opens a link in a new tab should be able to ask the engine
    // host for one; the host turns that into a tab in the LibGUI window.
    browser_options.allow_popups = WebView::AllowPopups::Yes;
}

}
