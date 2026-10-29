/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BrowserTab.h"
#include "DownloadsWindow.h"
#include "EngineClient.h"

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGUI/Action.h>
#include <LibGUI/Forward.h>
#include <LibGUI/TabWidget.h>
#include <LibCore/Forward.h>
#include <LibGUI/Window.h>

namespace LadybirdSDE {

class BrowserSession;

class BrowserWindow {
    AK_MAKE_NONCOPYABLE(BrowserWindow);

public:
    BrowserWindow(BrowserSession&, EngineClient&, u64 window_id, bool private_window);
    ~BrowserWindow();

    void show();

    void reopen_tab(String const& url);

    bool has_tab(u64 tab_id) const;
    bool has_request(u64 request_id) const;
    void adopt_engine_tab(u64 tab_id, u64 request_id);
    void handle_engine_tab_event(JsonObject const&, u64 tab_id);
    void handle_engine_global_event(JsonObject const&);
    void handle_engine_connected();
    void handle_engine_disconnected();
    void handle_engine_frame(u64 tab_id, Gfx::Bitmap const*, u64 sequence, Gfx::IntRect damage, bool full);
    void close_engine_tabs();

    Vector<String> tab_urls() const;
    size_t active_tab_index() const;
    Gfx::IntSize window_size() const;
    void set_window_size(Gfx::IntSize);
    Gfx::IntPoint window_position() const;
    void set_window_position(Gfx::IntPoint);
    bool is_maximized() const;
    void set_maximized(bool);
    void activate_tab_index(size_t);
    bool is_private() const { return m_private; }
    u64 id() const { return m_id; }

private:
    friend class BrowserSession;

    ErrorOr<void> initialize(Vector<String> initial_urls);
    void create_actions();
    void create_menus();
    void create_initial_tabs(Vector<String> const& initial_urls);

    BrowserTab& add_tab(String const& url, bool activate, bool request_engine_tab = true);
    void close_tab(BrowserTab&);
    void remove_tab(BrowserTab&);
    BrowserTab* active_tab() const;
    BrowserTab* tab_for_widget(GUI::Widget&) const;
    BrowserTab* tab_for_id(u64) const;
    void hard_reload_tab(BrowserTab&);
    BrowserTab* tab_for_request(u64) const;
    bool tab_is_live(BrowserTab const*) const;

    void activate_tab(BrowserTab&);
    void update_window_title();
    void update_action_state();
    void update_bookmark_action();
    void navigate_active(String const& url);
    void show_downloads();
    void update_downloads(JsonObject const&);
    void handle_download_event(JsonObject const&);
    void show_tab_context_menu(BrowserTab&, Gfx::IntPoint);
    void show_tab_search();
    void add_bookmark_menu_items(GUI::Menu&, Vector<BookmarkEntry> const&);
    void rebuild_bookmarks_menu();
    void close_other_tabs(BrowserTab&);
    void duplicate_tab(BrowserTab&);
    static ByteString string_for_key(JsonObject const&, StringView key);

    BrowserSession& m_session;
    EngineClient* m_engine { nullptr };
    u64 m_id { 0 };
    RefPtr<GUI::Window> m_window;
    RefPtr<GUI::TabWidget> m_tab_widget;

    RefPtr<GUI::Action> m_back_action;
    RefPtr<GUI::Action> m_forward_action;
    RefPtr<GUI::Action> m_reload_action;
    RefPtr<GUI::Action> m_reload_bypass_action;
    RefPtr<GUI::Action> m_stop_action;
    RefPtr<GUI::Action> m_new_tab_action;
    RefPtr<GUI::Action> m_home_action;
    RefPtr<GUI::Action> m_new_window_action;
    RefPtr<GUI::Action> m_new_private_window_action;
    RefPtr<GUI::Action> m_bookmarks_bar_action;
    RefPtr<GUI::Action> m_close_tab_action;
    RefPtr<GUI::Action> m_reopen_closed_tab_action;
    RefPtr<GUI::Action> m_next_tab_action;
    RefPtr<GUI::Action> m_previous_tab_action;
    RefPtr<GUI::Action> m_search_tabs_action;
    RefPtr<GUI::Action> m_bookmark_action;
    RefPtr<GUI::Action> m_find_action;
    RefPtr<GUI::Action> m_fullscreen_action;
    RefPtr<GUI::Action> m_cut_action;
    RefPtr<GUI::Action> m_copy_action;
    RefPtr<GUI::Action> m_paste_action;
    RefPtr<GUI::Action> m_select_all_action;
    RefPtr<GUI::Action> m_undo_action;
    RefPtr<GUI::Action> m_redo_action;
    RefPtr<GUI::Action> m_focus_location_action;
    RefPtr<GUI::Action> m_zoom_in_action;
    RefPtr<GUI::Action> m_zoom_out_action;
    RefPtr<GUI::Action> m_reset_zoom_action;
    RefPtr<GUI::Action> m_view_source_action;
    RefPtr<GUI::Action> m_downloads_action;
    RefPtr<GUI::Action> m_restore_session_action;

    Vector<NonnullOwnPtr<BrowserTab>> m_tabs;
    Vector<BookmarkEntry> m_bookmarks;
    Vector<DownloadEntry> m_downloads;
    RefPtr<DownloadsWindow> m_downloads_window;
    RefPtr<GUI::Menu> m_tab_context_menu;
    RefPtr<GUI::Menu> m_bookmarks_menu;
    bool m_show_bookmarks_bar { true };
    RefPtr<Gfx::Bitmap const> m_bookmark_icon;
    RefPtr<Gfx::Bitmap const> m_bookmark_filled_icon;

    bool m_private { false };
};

}
