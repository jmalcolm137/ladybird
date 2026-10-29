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
#include <LibGUI/Action.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Toolbar.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Forward.h>

namespace LadybirdSDE {

struct BookmarkEntry {
    String title;
    String url;
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
    bool can_go_back() const { return m_can_go_back; }
    bool can_go_forward() const { return m_can_go_forward; }
    bool is_bookmarked() const { return m_bookmarked; }

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
    void show_dialog(StringView kind, StringView message, StringView default_text = {});
    void show_context_menu(JsonArray const& items, Gfx::IntPoint screen_position);
    void show_permission_prompt(StringView name, StringView origin, u64 request_id);
    void show_file_picker(StringView filters);
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

private:
    static StringView button_name(GUI::MouseButton);
    void add_context_menu_items(GUI::Menu&, JsonArray const&);
    void add_select_dropdown_items(GUI::Menu&, JsonArray const&, bool in_group = false);

    EngineClient& m_engine;
    NonnullRefPtr<GUI::Widget> m_root;
    RefPtr<GUI::Toolbar> m_toolbar;
    RefPtr<GUI::TextBox> m_location_box;
    RefPtr<WebContentView> m_web_view;
    RefPtr<GUI::Widget> m_bookmarks_bar;
    RefPtr<GUI::Widget> m_find_bar;
    RefPtr<GUI::TextBox> m_find_box;
    RefPtr<GUI::Label> m_find_status;
    RefPtr<GUI::Statusbar> m_statusbar;
    RefPtr<GUI::Menu> m_context_menu;
    RefPtr<GUI::Menu> m_select_dropdown;
    Optional<u32> m_selected_select_item;
    bool m_select_dropdown_visible { false };

    u64 m_tab_id { 0 };
    u64 m_window_id { 0 };
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
