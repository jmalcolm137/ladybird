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
#include <LibWebView/SessionStore.h>

namespace LadybirdSDE {

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

WebView::SessionLaunchPolicy EngineApplication::launch_restore_policy() const
{
    if (WebView::Application::settings().restore_previous_session())
        return WebView::SessionLaunchPolicy::Everything;
    return WebView::SessionLaunchPolicy::PinnedOnly;
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
