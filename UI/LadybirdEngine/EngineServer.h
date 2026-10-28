/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/SelectItem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Forward.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>

namespace LadybirdSDE {

// One engine host per LibGUI window. The host owns the views; the chrome owns
// the tabs. The mapping is by id, and every event names the tab it belongs to.
class EngineServer : private WebView::FileDownloaderObserver {
    AK_MAKE_NONCOPYABLE(EngineServer);
    AK_ALLOC_WITH_KMALLOC;

public:
    EngineServer(NonnullOwnPtr<Core::LocalSocket> socket, ByteString frame_directory, Core::AnonymousBuffer theme);
    ~EngineServer();

    ErrorOr<void> start();
    void quit(int status = 0);

private:
    struct Tab {
        AK_ALLOC_WITH_KMALLOC;

        Tab(u64 id, u64 request_id, NonnullOwnPtr<WebView::HeadlessWebView> view, Gfx::IntSize viewport_size)
            : id(id)
            , request_id(request_id)
            , view(move(view))
            , viewport_size(viewport_size)
        {
        }

        u64 id { 0 };
        u64 request_id { 0 };
        u64 parent { 0 };
        NonnullOwnPtr<WebView::HeadlessWebView> view;
        Gfx::IntSize viewport_size;
        double last_reported_zoom { 1.0 };
        Gfx::IntSize last_reported_frame_size;
        // The frame buffer is one POSIX shared-memory object with a small ring
        // of fixed-size slots. The chrome maps the same object, so presenting a
        // frame costs one copy into the slot and one small control message.
        int frame_fd { -1 };
        void* frame_mapping { nullptr };
        size_t frame_mapping_size { 0 };
        size_t frame_slot_size { 0 };
        int frame_slots { 3 };
        int frame_write_slot { 0 };
        ByteString frame_buffer_name;
        u32 frame_sequence { 0 };
        bool focused { false };
        bool closing { false };
        Vector<NonnullRefPtr<WebView::Action>> context_menu_actions;
        HashMap<u64, Function<void(bool)>> pending_permissions;
    };

    void read_available();
    void dispatch_line(StringView);
    void dispatch_command(JsonObject const&);

    void handle_create_tab(JsonObject const&);
    void handle_activate_tab(u64 tab_id);
    void handle_navigate(u64 tab_id, JsonObject const&);
    void handle_resize(u64 tab_id, JsonObject const&);
    void handle_focus(u64 tab_id, JsonObject const&);
    void handle_mouse(u64 tab_id, JsonObject const&);
    void handle_key(u64 tab_id, JsonObject const&);
    void handle_wheel(u64 tab_id, JsonObject const&);
    void handle_dialog_response(u64 tab_id, JsonObject const&);
    void handle_close_tab(u64 tab_id);
    void handle_zoom(u64 tab_id, JsonObject const&);
    void handle_find(u64 tab_id, JsonObject const&);
    void handle_find_next(u64 tab_id, bool forward);
    void handle_toggle_bookmark(u64 tab_id);
    void handle_clipboard_copy(u64 tab_id, bool cut);
    void handle_paste(u64 tab_id, JsonObject const&);
    void handle_select_all(u64 tab_id);
    void handle_undo_redo(u64 tab_id, bool redo);
    void handle_context_menu_action(u64 tab_id, JsonObject const&);
    void handle_permission_response(u64 tab_id, JsonObject const&);
    void handle_file_picker_response(u64 tab_id, JsonObject const&);
    void handle_select_dropdown_closed(u64 tab_id, JsonObject const&);
    void handle_download_action(JsonObject const&);

    Tab* tab_for_id(u64 tab_id);
    Tab& create_tab(Gfx::IntSize viewport_size, bool is_private, u64 request_id = 0, u64 parent = 0);
    Tab& create_child_tab(Tab& parent, WebView::CanonicalTraversable&);
    void wire_tab(Tab&);
    void destroy_tab(u64 tab_id);
    bool ensure_frame_buffer(Tab&, Gfx::IntSize);
    void destroy_frame_buffer(Tab&);
    void send_frame(Tab&);
    void send_event(JsonObject);
    void flush_writes();
    void send_simple_event(StringView type, u64 tab_id);
    void send_tab_created(Tab&);
    void send_url(Tab&);
    void send_title(Tab&);
    void send_loading_state(Tab&, bool loading);
    void send_history_state(Tab&);
    void send_dialog(Tab&, StringView kind, StringView message, StringView default_text = {});
    void send_cursor(Tab&, Gfx::Cursor const&);
    void send_bookmarks();
    void send_context_menu(Tab&, StringView kind, Gfx::IntPoint, WebView::Menu&);
    void send_permission_request(Tab&, Utf16String const& name, URL::Origin const&, u64 request_id);
    void send_file_picker_request(Tab&, Web::HTML::FileFilter const&, Web::HTML::AllowMultipleFiles);
    void send_select_dropdown(Tab&, Gfx::IntPoint, i32 minimum_width, Vector<Web::HTML::SelectItem> const&);
    void send_downloads();
    void send_download_event(StringView type, WebView::FileDownloader::Download const&);
    JsonObject download_to_json(WebView::FileDownloader::Download const&);
    virtual void download_added(WebView::FileDownloader::Download const&) override;
    virtual void download_updated(WebView::FileDownloader::Download const&) override;
    virtual void download_removed(u64 id, WebView::IsPrivate) override;

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    ByteString m_frame_directory;
    Core::AnonymousBuffer m_theme;
    Vector<u8> m_read_buffer;
    Vector<u8> m_write_buffer;
    RefPtr<Core::Notifier> m_write_notifier;
    HashMap<u64, NonnullOwnPtr<Tab>> m_tabs;
    u64 m_next_tab_id { 1 };
    u64 m_active_tab_id { 0 };
    u64 m_next_frame_buffer_id { 1 };
    u64 m_next_permission_id { 1 };
    bool m_quitting { false };
};

}
