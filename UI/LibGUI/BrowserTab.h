/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "EngineClient.h"
#include "WebContentView.h"

#include <AK/Function.h>
#include <AK/JsonArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGUI/Action.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Forward.h>
#include <LibGUI/Label.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Toolbar.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Forward.h>

namespace LadybirdSDE {

struct InitialTab {
    String url;
    u64 session_tab_id { 0 };
    bool pinned { false };
};

struct BookmarkEntry {
    String id;
    String title;
    String url;
    bool is_folder { false };
    Vector<BookmarkEntry> children;
};

// One browser tab: a Serenity-style toolbar, the page view, and a status bar.
// The tab mirrors a view owned by the engine host.
class BrowserTab {
    AK_MAKE_NONCOPYABLE(BrowserTab);

public:
    BrowserTab(EngineClient&, GUI::Window&, GUI::Action& back_action, GUI::Action& forward_action, GUI::Action& reload_action, GUI::Action& stop_action, GUI::Action& bookmark_action, GUI::Action& home_action, GUI::Action& new_tab_action, GUI::Action& downloads_action);
    ~BrowserTab() = default;

    NonnullRefPtr<GUI::Widget> root() const { return m_root; }
    WebContentView& web_view() const { return *m_web_view; }

    u64 tab_id() const { return m_tab_id; }
    u64 request_id() const { return m_request_id; }
    bool is_attached() const { return m_tab_id != 0; }
    bool engine_tab_requested() const { return m_engine_tab_requested; }
    bool is_lazy() const { return m_lazy; }
    void set_lazy(bool lazy) { m_lazy = lazy; }
    u64 session_tab_id() const { return m_session_tab_id; }
    void set_session_tab_id(u64 session_tab_id) { m_session_tab_id = session_tab_id; }
    void set_session_window_id(u64 session_window_id) { m_session_window_id = session_window_id; }
    bool can_go_back() const { return m_can_go_back; }
    bool can_go_forward() const { return m_can_go_forward; }
    bool is_bookmarked() const { return m_bookmarked; }
    bool is_pinned() const { return m_pinned; }
    void set_pinned(bool pinned) { m_pinned = pinned; }
    bool is_muted() const { return m_muted; }
    bool is_audio_playing() const { return m_audio_playing; }
    void set_audio_state(bool playing, bool muted);
    Gfx::Bitmap const* favicon() const { return m_favicon.ptr(); }

    void set_window_id(u64 window_id) { m_window_id = window_id; }
    void create_engine_tab(u64 request_id, String const& url, bool private_tab = false);
    void set_initial_url(String const& url) { m_initial_url = url; }
    String const& initial_url() const { return m_initial_url; }
    void attach_tab_id(u64 tab_id);
    void detach();

    void navigate(String const& url);
    void reload();
    void stop();
    void go_back();
    void go_forward();
    void focus_location();
    void show_find_bar();
    void hide_find_bar();
    void set_bookmarked(bool bookmarked);
    void set_bookmarks(Vector<BookmarkEntry> const&);
    void set_bookmarks_bar_visible(bool visible);
    void set_loading(bool);
    void set_history(bool can_go_back, bool can_go_forward);
    void set_url(String const&);
    void set_title(String const&);
    void set_favicon(StringView path);
    void set_cursor(StringView name);
    void set_status(StringView);
    void set_crashed(bool crashed);
    void set_unresponsive(bool unresponsive);
    void set_find_result(u64 current, Optional<u64> total);
    void set_zoom_level(double zoom_level);
    void set_autocomplete_results(u64 request_id, JsonArray const& items, bool final);
    void show_site_info_dialog();
    void show_dialog(StringView kind, StringView message, StringView default_text = {});
    void show_context_menu(JsonArray const& items, Gfx::IntPoint screen_position);
    void show_permission_prompt(StringView name, StringView origin, u64 request_id);
    void show_file_picker(bool multiple, JsonArray const& filters);
    void set_permission_summary_provider(Function<String(String const&)> provider) { m_permission_summary_provider = move(provider); }
    void set_forget_permissions_handler(Function<void(String const&)> handler) { m_forget_permissions_handler = move(handler); }
    void show_select_dropdown(Gfx::IntPoint position, i32 minimum_width, JsonArray const& items);

    String const& display_title() const { return m_title; }
    String current_url() const;
    bool is_loading() const { return m_loading; }
    Gfx::IntSize content_size() const;

    Function<void(String const&)> on_title_change;
    Function<void(String const&)> on_url_change;
    Function<void(bool)> on_loading_change;
    Function<void(Gfx::Bitmap const*)> on_favicon_change;
    Function<void(bool)> on_bookmark_change;
    Function<void()> on_hard_reload;
    Function<void(String const&, String const&, bool)> on_permission_decision;
    Function<void(GUI::Widget&)> on_tabs_menu_request;

private:
    static StringView button_name(GUI::MouseButton);
    void add_context_menu_items(GUI::Menu&, JsonArray const&);
    void add_select_dropdown_items(GUI::Menu&, JsonArray const&, bool in_group = false);
    void add_bookmark_menu_items(GUI::Menu&, Vector<BookmarkEntry> const&);
    void update_site_button();
    void request_autocomplete();
    void request_find();
    void hide_autocomplete();
    void accept_autocomplete();
    void move_autocomplete_selection(int delta);

    EngineClient& m_engine;
    NonnullRefPtr<GUI::Widget> m_root;
    RefPtr<GUI::Toolbar> m_toolbar;
    RefPtr<GUI::Button> m_site_button;
    RefPtr<Gfx::Bitmap const> m_tabs_icon;
    RefPtr<Gfx::Bitmap const> m_site_settings_icon;
    RefPtr<Gfx::Bitmap const> m_warning_icon;
    RefPtr<GUI::TextBox> m_location_box;
    RefPtr<GUI::Button> m_zoom_button;
    RefPtr<WebContentView> m_web_view;
    RefPtr<GUI::Widget> m_bookmarks_bar;
    RefPtr<GUI::Widget> m_find_bar;
    RefPtr<GUI::TextBox> m_find_box;
    RefPtr<GUI::Label> m_find_status;
    RefPtr<GUI::Statusbar> m_statusbar;
    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Menu> m_select_dropdown;
    RefPtr<GUI::Menu> m_site_menu;
    RefPtr<GUI::CheckBox> m_match_case;
    RefPtr<Gfx::Bitmap> m_favicon;
    bool m_pinned { false };
    bool m_audio_playing { false };
    bool m_muted { false };
    Function<String(String const&)> m_permission_summary_provider;
    Function<void(String const&)> m_forget_permissions_handler;
    RefPtr<GUI::Menu> m_zoom_menu;
    RefPtr<GUI::Widget> m_autocomplete_overlay;
    RefPtr<GUI::TableView> m_autocomplete_table;
    RefPtr<GUI::JsonArrayModel> m_autocomplete_model;
    RefPtr<Core::Timer> m_autocomplete_timer;
    RefPtr<Core::Timer> m_autocomplete_hide_timer;
    JsonArray m_autocomplete_rows;
    Vector<String> m_autocomplete_texts;
    u64 m_autocomplete_request_id { 0 };
    bool m_autocomplete_visible { false };
    Optional<u32> m_selected_select_item;
    double m_zoom_level { 1.0 };
    bool m_select_dropdown_visible { false };

    u64 m_tab_id { 0 };
    u64 m_window_id { 0 };
    bool m_engine_tab_requested { false };
    bool m_lazy { false };
    u64 m_session_tab_id { 0 };
    u64 m_session_window_id { 0 };
    u64 m_request_id { 0 };
    String m_initial_url;
    String m_title;
    bool m_loading { false };
    bool m_can_go_back { false };
    bool m_can_go_forward { false };
    bool m_bookmarked { false };
    bool m_bookmarks_bar_visible { true };
    bool m_has_bookmarks { false };
    bool m_private { false };
};

}
