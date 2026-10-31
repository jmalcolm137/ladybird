/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserWindow.h"
#include "BrowserSession.h"

#include <AK/JsonArray.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/Icon.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/JsonArrayModel.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TableView.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGfx/Bitmap.h>

namespace LadybirdSDE {

static RefPtr<Gfx::Bitmap const> load_icon(StringView path)
{
    auto bitmap = Gfx::Bitmap::load_from_file(path);
    if (bitmap.is_error())
        return nullptr;
    return bitmap.release_value();
}

static BookmarkEntry bookmark_entry_from_json(JsonObject const& object)
{
    BookmarkEntry entry;
    entry.id = String::from_byte_string(object.get_byte_string("id"sv).value_or({})).release_value();
    entry.title = String::from_byte_string(object.get_byte_string("title"sv).value_or({})).release_value();
    entry.url = String::from_byte_string(object.get_byte_string("url"sv).value_or({})).release_value();
    entry.is_folder = object.get_byte_string("type"sv).value_or({}) == "folder"sv;

    if (auto items = object.get_array("items"sv); items.has_value()) {
        for (auto const& value : items->values()) {
            if (value.is_object())
                entry.children.append(bookmark_entry_from_json(value.as_object()));
        }
    }

    return entry;
}

static BookmarkEntry const* find_bookmark(Vector<BookmarkEntry> const& items, StringView url)
{
    for (auto const& item : items) {
        if (item.is_folder) {
            if (auto const* found = find_bookmark(item.children, url))
                return found;
        } else if (item.url == url) {
            return &item;
        }
    }
    return nullptr;
}

static DownloadEntry download_entry_from_json(JsonObject const& object)
{
    DownloadEntry entry;
    entry.id = object.get_u64("id"sv).value_or(0);
    entry.url = String::from_byte_string(object.get_byte_string("url"sv).value_or({})).release_value();
    entry.destination = String::from_byte_string(object.get_byte_string("destination"sv).value_or({})).release_value();
    entry.status = String::from_byte_string(object.get_byte_string("status"sv).value_or({})).release_value();
    entry.downloaded = object.get_u64("downloaded"sv).value_or(0);
    if (auto total = object.get_u64("total"sv); total.has_value())
        entry.total = *total;
    if (auto progress = object.get_double_with_precision_loss("progress"sv); progress.has_value())
        entry.progress = *progress;
    entry.can_resume = object.get_bool("can_resume"sv).value_or(false);
    entry.is_private = object.get_bool("private"sv).value_or(false);
    entry.error = String::from_byte_string(object.get_byte_string("error"sv).value_or({})).release_value();
    return entry;
}

BrowserWindow::BrowserWindow(BrowserSession& session, EngineClient& engine, u64 window_id, bool private_window, u64 session_window_id)
    : m_session(session)
    , m_engine(&engine)
    , m_id(window_id)
    , m_session_window_id(session_window_id)
    , m_private(private_window)
{
}

BrowserWindow::~BrowserWindow() = default;

ByteString BrowserWindow::string_for_key(JsonObject const& object, StringView key)
{
    if (auto value = object.get_byte_string(key); value.has_value())
        return value.release_value();
    return {};
}

ErrorOr<void> BrowserWindow::initialize(Vector<String> initial_urls, Optional<size_t> active_tab, bool lazy_load)
{
    Vector<InitialTab> initial_tabs;
    for (auto& url : initial_urls)
        initial_tabs.append({ move(url), 0 });
    return initialize(move(initial_tabs), active_tab, lazy_load);
}

ErrorOr<void> BrowserWindow::initialize(Vector<InitialTab> initial_tabs, Optional<size_t> active_tab, bool lazy_load)
{
    m_window = GUI::Window::construct();
    m_window->set_title(m_private ? "Ladybird (Private)"sv : "Ladybird"sv);
    m_window->resize(1000, 520);
    m_window->set_icon(GUI::Icon::default_icon("app-browser"sv).bitmap_for_size(32));

    auto main_widget = m_window->set_main_widget<GUI::Widget>();
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins {}, 0);

    m_devtools_bar = main_widget->add<GUI::Widget>();
    m_devtools_bar->set_fixed_height(30);
    m_devtools_bar->set_fill_with_background_color(true);
    m_devtools_bar->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 8, 3 }, 6);
    m_devtools_bar->set_visible(false);

    m_devtools_label = m_devtools_bar->add<GUI::Label>(""_string);
    m_devtools_label->set_preferred_width(GUI::SpecialDimension::Grow);
    m_devtools_label->set_text_alignment(Gfx::TextAlignment::CenterLeft);

    m_tab_widget = main_widget->add<GUI::TabWidget>();
    m_tab_widget->set_preferred_size(GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow);
    m_tab_widget->set_container_margins(GUI::Margins {});
    m_tab_widget->set_uniform_tabs(true);
    m_tab_widget->set_close_button_enabled(true);
    m_tab_widget->set_text_alignment(Gfx::TextAlignment::CenterLeft);
    m_tab_widget->set_reorder_allowed(true);

    create_actions();
    create_menus();

    m_history_refresh_timer = Core::Timer::create_single_shot(400, [this] {
        if (m_engine && m_engine->is_connected())
            m_engine->request_history(10);
    });

    m_tab_widget->on_tab_close_click = [this](GUI::Widget& widget) {
        if (auto* tab = tab_for_widget(widget))
            close_tab(*tab);
    };
    m_tab_widget->on_change = [this](GUI::Widget& widget) {
        if (auto* tab = tab_for_widget(widget))
            activate_tab(*tab);
    };
    m_tab_widget->on_middle_click = [this](GUI::Widget& widget) {
        if (auto* tab = tab_for_widget(widget))
            close_tab(*tab);
    };
    m_tab_widget->on_context_menu_request = [this](GUI::Widget& widget, GUI::ContextMenuEvent const& event) {
        if (auto* tab = tab_for_widget(widget)) {
            auto origin = m_tab_widget->screen_relative_rect().location();
            show_tab_context_menu(*tab, event.screen_position().translated(-origin.x(), -origin.y()));
        }
    };

    m_window->on_close = [this] {
        m_session.window_will_close(*this);
    };

    create_initial_tabs(initial_tabs, active_tab, lazy_load);
    set_devtools_state(m_session.devtools_port());
    return {};
}

void BrowserWindow::create_actions()
{
    auto back_icon = load_icon("/res/icons/16x16/go-back.png"sv);
    auto forward_icon = load_icon("/res/icons/16x16/go-forward.png"sv);
    auto reload_icon = load_icon("/res/icons/16x16/reload.png"sv);
    auto stop_icon = load_icon("/res/icons/16x16/stop.png"sv);
    auto new_tab_icon = load_icon("/res/icons/16x16/new-tab.png"sv);
    auto close_tab_icon = load_icon("/res/icons/16x16/close-tab.png"sv);
    m_bookmark_icon = load_icon("/res/icons/16x16/bookmark-contour.png"sv);
    m_bookmark_filled_icon = load_icon("/res/icons/16x16/bookmark-filled.png"sv);
    m_pinned_tab_icon = load_icon("/res/icons/16x16/window-pin.png"sv);
    m_muted_tab_icon = load_icon("/res/icons/16x16/audio-volume-muted.png"sv);
    m_playing_tab_icon = load_icon("/res/icons/16x16/audio-volume-high.png"sv);

    m_back_action = GUI::Action::create("Back"sv, { Mod_Alt, Key_Left }, back_icon, [this](auto&) {
        if (auto* tab = active_tab())
            tab->go_back();
    }, m_window.ptr());
    m_back_action->set_status_tip("Go back"_string);

    m_forward_action = GUI::Action::create("Forward"sv, { Mod_Alt, Key_Right }, forward_icon, [this](auto&) {
        if (auto* tab = active_tab())
            tab->go_forward();
    }, m_window.ptr());
    m_forward_action->set_status_tip("Go forward"_string);

    m_reload_action = GUI::Action::create("Reload"sv, { Mod_Ctrl, Key_R }, Key_F5, reload_icon, [this](auto&) {
        if (auto* tab = active_tab())
            tab->reload();
    }, m_window.ptr());
    m_reload_action->set_status_tip("Reload the page"_string);

    m_reload_bypass_action = GUI::Action::create("Reload &Bypassing Cache"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_R }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->reload(tab->tab_id(), true);
    }, m_window.ptr());

    m_stop_action = GUI::Action::create("Stop"sv, stop_icon, [this](auto&) {
        if (auto* tab = active_tab())
            tab->stop();
    }, m_window.ptr());
    m_stop_action->set_status_tip("Stop loading"_string);

    m_new_tab_action = GUI::Action::create("&New Tab"sv, { Mod_Ctrl, Key_T }, new_tab_icon, [this](auto&) {
        add_tab(String {}, true);
    }, m_window.ptr());
    m_new_tab_action->set_status_tip("Open a new tab"_string);

    m_home_action = GUI::Action::create("&Home"sv, load_icon("/res/icons/16x16/go-home.png"sv), [this](auto&) {
        navigate_active(String {});
    }, m_window.ptr());
    m_home_action->set_status_tip("Open the new tab page"_string);

    m_new_window_action = GUI::Action::create("New &Window"sv, { Mod_Ctrl, Key_N }, [this](auto&) {
        m_session.create_window({}, false);
    }, m_window.ptr());
    m_new_private_window_action = GUI::Action::create("New &Private Window"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_N }, [this](auto&) {
        m_session.create_window({}, true);
    }, m_window.ptr());

    m_bookmarks_bar_action = GUI::Action::create_checkable("Show &Bookmarks Bar"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_B }, [this](auto& action) {
        m_show_bookmarks_bar = action.is_checked();
        for (auto& tab : m_tabs)
            tab->set_bookmarks_bar_visible(m_show_bookmarks_bar);
    }, m_window.ptr());
    m_bookmarks_bar_action->set_checked(true);

    m_close_tab_action = GUI::Action::create("&Close Tab"sv, { Mod_Ctrl, Key_W }, close_tab_icon, [this](auto&) {
        if (auto* tab = active_tab())
            close_tab(*tab);
    }, m_window.ptr());
    m_close_tab_action->set_status_tip("Close the current tab"_string);

    m_reopen_closed_tab_action = GUI::Action::create("&Reopen Closed Tab"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_T }, [this](auto&) {
        m_session.reopen_closed_tab();
    }, m_window.ptr());
    m_reopen_closed_tab_action->set_status_tip("Reopen the most recently closed tab"_string);

    m_next_tab_action = GUI::Action::create("&Next Tab"sv, GUI::Shortcut(Mod_Ctrl, Key_PageDown), GUI::Shortcut(Mod_Ctrl, Key_Tab), [this](auto&) {
        m_tab_widget->activate_next_tab();
    }, m_window.ptr());
    m_previous_tab_action = GUI::Action::create("&Previous Tab"sv, GUI::Shortcut(static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_PageUp), [this](auto&) {
        m_tab_widget->activate_previous_tab();
    }, m_window.ptr());

    m_search_tabs_action = GUI::Action::create("&Search Tabs…"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_A }, [this](auto&) {
        show_tab_search();
    }, m_window.ptr());
    m_search_tabs_action->set_status_tip("Find an open tab"_string);

    m_bookmark_action = GUI::Action::create_checkable("&Bookmark This Page"sv, { Mod_Ctrl, Key_D }, m_bookmark_icon, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->toggle_bookmark(tab->tab_id());
    }, m_window.ptr());
    m_bookmark_action->set_status_tip("Add or remove this page from bookmarks"_string);
    m_bookmark_action->set_enabled(false);

    m_find_action = GUI::Action::create("&Find in Page"sv, { Mod_Ctrl, Key_F }, [this](auto&) {
        if (auto* tab = active_tab())
            tab->show_find_bar();
    }, m_window.ptr());
    m_find_action->set_status_tip("Find text on this page"_string);

    m_fullscreen_action = GUI::Action::create("Toggle &Fullscreen"sv, GUI::Shortcut { Key_F11 }, [this](auto&) {
        m_window->set_fullscreen(!m_window->is_fullscreen());
    }, m_window.ptr());
    m_fullscreen_action->set_status_tip("Toggle fullscreen mode"_string);

    m_focus_location_action = GUI::Action::create("Focus &Location"sv, { Mod_Ctrl, Key_L }, Key_F6, [this](auto&) {
        if (auto* tab = active_tab())
            tab->focus_location();
    }, m_window.ptr());
    m_focus_location_action->set_status_tip("Focus the location bar"_string);

    m_zoom_in_action = GUI::Action::create("Zoom &In"sv, { Mod_Ctrl, Key_Plus }, Key_Equal, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->set_zoom(tab->tab_id(), "in"sv);
    }, m_window.ptr());
    m_zoom_out_action = GUI::Action::create("Zoom &Out"sv, { Mod_Ctrl, Key_Minus }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->set_zoom(tab->tab_id(), "out"sv);
    }, m_window.ptr());
    m_reset_zoom_action = GUI::Action::create("&Reset Zoom"sv, { Mod_Ctrl, Key_0 }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->set_zoom(tab->tab_id(), "reset"sv);
    }, m_window.ptr());

    m_cut_action = GUI::Action::create("Cu&t"sv, { Mod_Ctrl, Key_X }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->copy(tab->tab_id(), true);
    }, m_window.ptr());
    m_copy_action = GUI::Action::create("&Copy"sv, { Mod_Ctrl, Key_C }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->copy(tab->tab_id(), false);
    }, m_window.ptr());
    m_paste_action = GUI::Action::create("&Paste"sv, { Mod_Ctrl, Key_V }, [this](auto&) {
        auto* tab = active_tab();
        if (!tab || !tab->is_attached())
            return;
        auto items = m_session.clipboard_items_for_paste();
        if (items.is_empty())
            return;
        m_engine->paste(tab->tab_id(), move(items));
    }, m_window.ptr());
    m_select_all_action = GUI::Action::create("Select &All"sv, { Mod_Ctrl, Key_A }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->select_all(tab->tab_id());
    }, m_window.ptr());
    m_undo_action = GUI::Action::create("&Undo"sv, { Mod_Ctrl, Key_Z }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->undo(tab->tab_id());
    }, m_window.ptr());
    m_redo_action = GUI::Action::create("&Redo"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_Z }, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->redo(tab->tab_id());
    }, m_window.ptr());

    m_view_source_action = GUI::Action::create("View &Source"sv, { Mod_Ctrl, Key_U }, [this](auto&) {
        if (auto* tab = active_tab(); tab && !tab->current_url().is_empty())
            navigate_active(String::formatted("view-source:{}", tab->current_url()).release_value());
    }, m_window.ptr());
    m_view_source_action->set_status_tip("View the source of the current page"_string);

    m_downloads_action = GUI::Action::create("&Downloads"sv, load_icon("/res/icons/16x16/download.png"sv), [this](auto&) {
        show_downloads();
    }, m_window.ptr());
    m_downloads_action->set_status_tip("Show downloads"_string);

    m_site_info_action = GUI::Action::create("Site &Information"sv, [this](auto&) {
        if (auto* tab = active_tab())
            tab->show_site_info_dialog();
    }, m_window.ptr());
    m_site_info_action->set_status_tip("Show connection and zoom information"_string);

    m_toggle_devtools_action = GUI::Action::create("Enable &DevTools"sv, { static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_I }, { 0, Key_F12 }, [this](auto&) {
        m_session.toggle_devtools();
    }, m_window.ptr());
    m_toggle_devtools_action->set_status_tip("Enable or disable the DevTools server"_string);

    m_open_devtools_client_action = GUI::Action::create("Open DevTools &Client"sv, [this](auto&) {
        m_session.open_devtools_client();
    }, m_window.ptr());
    m_open_devtools_client_action->set_status_tip("Open a Firefox DevTools client for the active tab"_string);
    m_open_devtools_client_action->set_enabled(false);

    m_back_action->set_enabled(false);
    m_forward_action->set_enabled(false);
}

void BrowserWindow::create_menus()
{
    auto& file_menu = *m_window->add_menu("&File"_string);
    file_menu.add_action(*m_new_tab_action);
    file_menu.add_action(*m_new_window_action);
    file_menu.add_action(*m_new_private_window_action);
    file_menu.add_action(*m_close_tab_action);
    file_menu.add_action(*m_reopen_closed_tab_action);
    file_menu.add_separator();
    file_menu.add_action(*m_next_tab_action);
    file_menu.add_action(*m_previous_tab_action);
    file_menu.add_action(*m_search_tabs_action);
    file_menu.add_separator();
    file_menu.add_action(GUI::CommonActions::make_quit_action([this](auto&) {
        m_session.quit();
    }));

    auto& edit_menu = *m_window->add_menu("&Edit"_string);
    edit_menu.add_action(*m_undo_action);
    edit_menu.add_action(*m_redo_action);
    edit_menu.add_separator();
    edit_menu.add_action(*m_cut_action);
    edit_menu.add_action(*m_copy_action);
    edit_menu.add_action(*m_paste_action);
    edit_menu.add_separator();
    edit_menu.add_action(*m_select_all_action);
    edit_menu.add_action(*m_find_action);

    auto& view_menu = *m_window->add_menu("&View"_string);
    view_menu.add_action(*m_home_action);
    view_menu.add_action(*m_reload_action);
    view_menu.add_action(*m_reload_bypass_action);
    view_menu.add_action(*m_stop_action);
    view_menu.add_separator();
    view_menu.add_action(*m_view_source_action);
    view_menu.add_action(*m_zoom_in_action);
    view_menu.add_action(*m_zoom_out_action);
    view_menu.add_action(*m_reset_zoom_action);
    view_menu.add_separator();
    view_menu.add_action(*m_find_action);
    view_menu.add_action(*m_fullscreen_action);
    view_menu.add_action(*m_bookmarks_bar_action);

    auto& bookmarks_menu = *m_window->add_menu("&Bookmarks"_string);
    bookmarks_menu.add_action(*m_bookmark_action);
    bookmarks_menu.add_separator();
    bookmarks_menu.add_action(GUI::Action::create("Add Bookmark…"sv, [this](auto&) {
        show_bookmark_dialog(false);
    }, m_window.ptr()));
    bookmarks_menu.add_action(GUI::Action::create("Edit Bookmark…"sv, [this](auto&) {
        show_bookmark_dialog(true);
    }, m_window.ptr()));
    bookmarks_menu.add_action(GUI::Action::create("Remove Bookmark"sv, [this](auto&) {
        auto* tab = active_tab();
        if (!tab)
            return;
        auto id = bookmark_id_for_url(tab->current_url());
        if (id.is_empty())
            return;
        m_engine->remove_bookmark(tab->tab_id(), id);
    }, m_window.ptr()));
    bookmarks_menu.add_separator();
    bookmarks_menu.add_action(GUI::Action::create("&Manage Bookmarks…"sv, [this](auto&) {
        show_bookmarks_manager();
    }, m_window.ptr()));
    bookmarks_menu.add_action(GUI::Action::create("Show &Bookmarks"sv, [this](auto&) {
        navigate_active("about:bookmarks"_string);
    }, m_window.ptr()));
    bookmarks_menu.add_separator();
    m_bookmarks_menu = bookmarks_menu.add_submenu("All &Bookmarks"_string);

    m_tabs_menu = m_window->add_menu("&Tabs"_string);
    // The toolbar button gets its own menu object: reusing the menubar menu
    // makes the popup render the menubar's button strip as extra rows.
    m_tabs_popup_menu = GUI::Menu::construct("Tabs"_string);
    rebuild_tabs_menu();

    m_history_menu = m_window->add_menu("&History"_string);
    rebuild_history_menu();

    auto& tools_menu = *m_window->add_menu("&Tools"_string);
    tools_menu.add_action(GUI::Action::create("&Settings"sv, [this](auto&) {
        navigate_active("about:settings"_string);
    }, m_window.ptr()));
    tools_menu.add_action(*m_site_info_action);
    tools_menu.add_action(*m_downloads_action);
    tools_menu.add_separator();
    // Keep these flat: activating an item in a nested LibGUI submenu tears the parent popup down in an order the
    // SDE compositor rejects.
    tools_menu.add_action(*m_toggle_devtools_action);
    tools_menu.add_action(*m_open_devtools_client_action);

    m_report_website_issue_action = GUI::Action::create("Report Website &Issue…"sv, [this](auto&) {
        if (auto* tab = active_tab(); tab && tab->is_attached())
            m_engine->compat_report_begin(tab->tab_id());
    }, m_window.ptr());
    m_report_website_issue_action->set_enabled(false);

    auto& help_menu = *m_window->add_menu("&Help"_string);
    help_menu.add_action(*m_report_website_issue_action);
    help_menu.add_action(GUI::CommonActions::make_about_action("Ladybird"_string, GUI::Icon::default_icon("app-browser"sv), m_window.ptr()));
}

void BrowserWindow::create_initial_tabs(Vector<InitialTab> const& initial_tabs, Optional<size_t> active_tab, bool lazy_load)
{
    if (initial_tabs.is_empty()) {
        add_tab(String {}, true);
        return;
    }

    auto active_index = active_tab.value_or(initial_tabs.size() - 1);
    if (active_index >= initial_tabs.size())
        active_index = initial_tabs.size() - 1;

    for (size_t i = 0; i < initial_tabs.size(); ++i) {
        auto is_active = i == active_index;
        auto& tab = add_tab(initial_tabs[i].url, is_active, is_active || !lazy_load, lazy_load && !is_active, initial_tabs[i].session_tab_id);
        tab.set_pinned(initial_tabs[i].pinned);
    }
}

void BrowserWindow::set_devtools_state(Optional<u16> port)
{
    auto enabled = port.has_value();
    m_toggle_devtools_action->set_text(enabled ? "Disable &DevTools"sv : "Enable &DevTools"sv);
    m_open_devtools_client_action->set_enabled(enabled);

    if (m_devtools_label && enabled)
        m_devtools_label->set_text(String::formatted("DevTools is enabled on port {} (Tools > Open DevTools Client)", *port).release_value());
    if (m_devtools_bar) {
        m_devtools_bar->set_visible(enabled);
        if (auto* parent = m_devtools_bar->parent_widget())
            parent->invalidate_layout();
    }
    if (m_window)
        m_window->update();
}

void BrowserWindow::show_engine_error(String const& message)
{
    // This arrives in response to a menu action, while the menu popup is still being torn down. A modal dialog
    // there re-enters the menu's event handling; the status bar is a safe place for a short-lived error.
    if (auto* tab = active_tab())
        tab->set_status(message);
    else
        warnln("Ladybird: {}", message);
}

void BrowserWindow::set_session_window_id(u64 session_window_id)
{
    m_session_window_id = session_window_id;
    for (auto& tab : m_tabs)
        tab->set_session_window_id(session_window_id);

    // Legacy URL-only sessions restore background tabs lazily. Register those
    // tabs with the native session store now, without loading them, so the next
    // launch can restore them from session_plan.
    for (auto& tab : m_tabs) {
        if (!tab->is_lazy() || tab->session_tab_id() != 0)
            continue;
        auto request_id = m_session.next_request_id();
        m_pending_session_tab_requests.set(request_id, tab.ptr());
        m_engine->register_session_tab(session_window_id, tab->initial_url(), request_id, m_id);
    }
}

void BrowserWindow::handle_session_tab_registered(u64 request_id, u64 session_tab_id)
{
    auto tab = m_pending_session_tab_requests.take(request_id);
    if (tab.has_value())
        (*tab)->set_session_tab_id(session_tab_id);
}

void BrowserWindow::reopen_tab(String const& url)
{
    add_tab(url, true);
}

void BrowserWindow::open_startup_urls(Vector<String> const& urls)
{
    // The reference frontend opens URLs passed on the command line in the
    // restored window, as tabs after the restored ones.
    for (size_t i = 0; i < urls.size(); ++i)
        add_tab(urls[i], i == 0);
}

BrowserTab& BrowserWindow::add_tab(String const& url, bool activate, bool request_engine_tab, bool lazy, u64 session_tab_id)
{
    auto tab = make<BrowserTab>(*m_engine, *m_window, *m_back_action, *m_forward_action, *m_reload_action, *m_stop_action, *m_bookmark_action, *m_home_action, *m_new_tab_action, *m_downloads_action);
    auto* tab_pointer = tab.ptr();
    tab->set_window_id(m_id);
    tab->set_initial_url(url);
    tab->set_lazy(lazy);
    tab->set_session_tab_id(session_tab_id);
    tab->set_session_window_id(m_session_window_id);

    tab->on_title_change = [this, tab_pointer](String const& title) {
        if (!tab_is_live(tab_pointer))
            return;
        m_tab_widget->set_tab_title(*tab_pointer->root(), title);
        rebuild_tabs_menu();
        if (active_tab() == tab_pointer)
            update_window_title();
    };
    tab->on_url_change = [this, tab_pointer](String const&) {
        if (!tab_is_live(tab_pointer))
            return;
        if (active_tab() == tab_pointer)
            update_window_title();
    };
    tab->on_bookmark_change = [this, tab_pointer](bool) {
        if (!tab_is_live(tab_pointer))
            return;
        if (active_tab() == tab_pointer)
            update_bookmark_action();
    };
    tab->on_favicon_change = [this, tab_pointer](Gfx::Bitmap const*) {
        if (!tab_is_live(tab_pointer))
            return;
        update_tab_icon(*tab_pointer);
    };
    tab->on_loading_change = [this, tab_pointer](bool) {
        if (!tab_is_live(tab_pointer))
            return;
        if (active_tab() == tab_pointer)
            update_window_title();
    };
    tab->on_hard_reload = [this, tab_pointer] {
        if (tab_is_live(tab_pointer))
            hard_reload_tab(*tab_pointer);
    };
    tab->set_permission_summary_provider([this](String const& origin) {
        return m_session.permission_summary(origin);
    });
    tab->set_forget_permissions_handler([this](String const& origin) {
        m_session.forget_permissions(origin);
    });
    tab->on_permission_decision = [this](String const& origin, String const& name, bool granted, bool remember) {
        m_session.note_permission(origin, name, granted, remember && !m_private);
    };
    tab->on_certificate_request = [this, tab_pointer] {
        if (tab_is_live(tab_pointer) && tab_pointer->is_attached())
            m_engine->request_certificate_info(tab_pointer->tab_id());
    };
    tab->on_tabs_menu_request = [this](GUI::Widget& anchor) {
        rebuild_tabs_menu();
        if (m_tabs_popup_menu)
            m_tabs_popup_menu->popup(anchor.screen_relative_rect().bottom_left(), {}, anchor.rect());
    };

    m_tabs.append(move(tab));
    tab_pointer->set_bookmarks(m_bookmarks);
    tab_pointer->set_bookmarks_bar_visible(m_show_bookmarks_bar);

    // A restored tab that has not loaded yet shows its URL until the page
    // provides a title, matching the reference frontend.
    auto tab_title = lazy && !url.is_empty() ? url : "New Tab"_string;
    m_tab_widget->add_tab(tab_pointer->root(), tab_title);

    if (request_engine_tab) {
        if (m_engine->is_connected())
            tab_pointer->create_engine_tab(m_session.next_request_id(), url, m_private);
        else
            tab_pointer->set_status("Starting the Ladybird engine..."sv);
    } else if (lazy) {
        tab_pointer->set_status("Not loaded"sv);
    }

    if (activate)
        m_tab_widget->set_active_widget(tab_pointer->root().ptr());

    update_action_state();
    update_window_title();
    rebuild_tabs_menu();
    return *tab_pointer;
}

void BrowserWindow::close_tab(BrowserTab& tab)
{
    auto url = tab.current_url();
    if (!url.is_empty())
        m_session.note_closed_tab(url, tab.display_title(), m_private);

    if (m_tabs.size() == 1) {
        // Closing the last tab closes the window, like the SerenityOS Browser.
        m_window->close();
        return;
    }

    if (tab.is_attached())
        m_engine->close_tab(tab.tab_id());
    remove_tab(tab);
}

void BrowserWindow::remove_tab(BrowserTab& tab)
{
    auto* tab_pointer = &tab;
    auto root = tab.root();
    tab.detach();

    m_tabs.remove_first_matching([tab_pointer](auto const& candidate) {
        return candidate.ptr() == tab_pointer;
    });
    m_tab_widget->remove_widget(*root);

    update_action_state();
    update_window_title();
    rebuild_tabs_menu();
}

GUI::Widget* BrowserWindow::focused_widget() const
{
    return m_window ? m_window->focused_widget() : nullptr;
}

BrowserTab* BrowserWindow::active_tab() const
{
    auto* widget = m_tab_widget->active_widget();
    if (!widget)
        return m_tabs.is_empty() ? nullptr : m_tabs.last().ptr();
    return tab_for_widget(*widget);
}

BrowserTab* BrowserWindow::tab_for_widget(GUI::Widget& widget) const
{
    for (auto const& tab : m_tabs) {
        if (tab->root().ptr() == &widget)
            return tab.ptr();
    }
    return nullptr;
}

BrowserTab* BrowserWindow::tab_for_id(u64 tab_id) const
{
    for (auto const& tab : m_tabs) {
        if (tab->tab_id() == tab_id)
            return tab.ptr();
    }
    return nullptr;
}

BrowserTab* BrowserWindow::tab_for_request(u64 request_id) const
{
    for (auto const& tab : m_tabs) {
        if (tab->request_id() == request_id)
            return tab.ptr();
    }
    return nullptr;
}

void BrowserWindow::hard_reload_tab(BrowserTab& tab)
{
    auto url = tab.current_url();
    if (url.is_empty())
        url = tab.initial_url();

    if (tab.is_attached())
        m_engine->close_tab(tab.tab_id());
    tab.detach();
    tab.set_unresponsive(false);
    tab.set_status("Reloading..."sv);
    tab.create_engine_tab(m_session.next_request_id(), url, m_private);
}

bool BrowserWindow::tab_is_live(BrowserTab const* tab) const
{
    for (auto const& candidate : m_tabs) {
        if (candidate.ptr() == tab)
            return true;
    }
    return false;
}

void BrowserWindow::activate_tab(BrowserTab& tab)
{
    if (m_reordering_tabs)
        return;
    ensure_tab_loaded(tab);
    if (tab.is_attached())
        m_engine->activate_tab(tab.tab_id(), m_id);
    // A background tab may have received only damage updates. Repaint the
    // complete last frame when it becomes visible again.
    tab.web_view().update();
    update_action_state();
    update_window_title();
}

void BrowserWindow::update_window_title()
{
    auto* tab = active_tab();
    if (!tab) {
        m_window->set_title(m_private ? "Ladybird (Private)"sv : "Ladybird"sv);
        return;
    }

    auto title = tab->display_title();
    if (title.is_empty())
        title = "New Tab"_string;
    m_window->set_title(m_private ? ByteString::formatted("{} - Ladybird (Private)", title.bytes_as_string_view()) : ByteString::formatted("{} - Ladybird", title.bytes_as_string_view()));
}

void BrowserWindow::update_action_state()
{
    auto* tab = active_tab();
    m_back_action->set_enabled(tab && tab->can_go_back());
    m_forward_action->set_enabled(tab && tab->can_go_forward());
    m_reload_action->set_enabled(tab != nullptr);
    m_stop_action->set_enabled(tab != nullptr);
    m_close_tab_action->set_enabled(tab != nullptr);
    m_reopen_closed_tab_action->set_enabled(m_session.closed_tab_count() > 0);
    m_bookmark_action->set_enabled(tab != nullptr);
    m_site_info_action->set_enabled(tab != nullptr);
    if (m_report_website_issue_action) {
        auto url = tab ? tab->current_url() : String {};
        auto reportable = url.bytes_as_string_view().starts_with("http:"sv) || url.bytes_as_string_view().starts_with("https:"sv);
        m_report_website_issue_action->set_enabled(tab && tab->is_attached() && reportable);
    }
    m_find_action->set_enabled(tab != nullptr);
    m_zoom_in_action->set_enabled(tab != nullptr);
    m_zoom_out_action->set_enabled(tab != nullptr);
    m_reset_zoom_action->set_enabled(tab != nullptr);
    update_bookmark_action();
}

void BrowserWindow::update_bookmark_action()
{
    auto* tab = active_tab();
    auto bookmarked = tab && tab->is_bookmarked();
    m_bookmark_action->set_checked(bookmarked);
    m_bookmark_action->set_icon(bookmarked ? m_bookmark_filled_icon.ptr() : m_bookmark_icon.ptr());
}

void BrowserWindow::navigate_active(String const& url)
{
    if (auto* tab = active_tab())
        tab->navigate(url);
}

void BrowserWindow::handle_engine_connected()
{
    if (m_engine)
        m_engine->request_history(10);
    for (auto& tab : m_tabs) {
        if (!tab->is_attached() && !tab->is_lazy()) {
            tab->create_engine_tab(m_session.next_request_id(), tab->initial_url(), m_private);
            tab->set_status(""sv);
        }
    }
}

void BrowserWindow::ensure_tab_loaded(BrowserTab& tab)
{
    if (tab.is_attached() || tab.engine_tab_requested())
        return;

    tab.set_lazy(false);
    tab.set_session_window_id(m_session_window_id);
    if (m_engine->is_connected())
        tab.create_engine_tab(m_session.next_request_id(), tab.initial_url(), m_private);
    else
        tab.set_status("Starting the Ladybird engine..."sv);
}

bool BrowserWindow::has_tab(u64 tab_id) const
{
    return tab_for_id(tab_id) != nullptr;
}

bool BrowserWindow::has_request(u64 request_id) const
{
    return tab_for_request(request_id) != nullptr;
}

void BrowserWindow::adopt_engine_tab(u64 tab_id, u64 request_id)
{
    auto* tab = request_id != 0 ? tab_for_request(request_id) : nullptr;
    if (!tab) {
        // A tab the engine or a page opened on behalf of this window.
        auto& new_tab = add_tab(String {}, true, false);
        tab = &new_tab;
    }

    tab->attach_tab_id(tab_id);
    m_tab_widget->set_active_widget(tab->root().ptr());
    update_action_state();
}

void BrowserWindow::handle_engine_global_event(JsonObject const& event)
{
    auto type = string_for_key(event, "type"sv);

    if (type == "downloads"sv) {
        Vector<DownloadEntry> downloads;
        if (auto items = event.get_array("items"sv); items.has_value()) {
            for (auto const& value : items->values()) {
                if (value.is_object())
                    downloads.append(download_entry_from_json(value.as_object()));
            }
        }
        m_downloads = move(downloads);
        if (m_downloads_window)
            m_downloads_window->set_downloads(m_downloads);
        return;
    }

    if (type == "download_added"sv || type == "download_updated"sv || type == "download_removed"sv) {
        handle_download_event(event);
        return;
    }

    if (type == "history_recent"sv) {
        m_recent_history.clear();
        if (auto items = event.get_array("items"sv); items.has_value()) {
            for (auto const& value : items->values()) {
                if (!value.is_object())
                    continue;
                auto const& object = value.as_object();
                RecentHistoryEntry entry;
                entry.url = String::from_byte_string(object.get_byte_string("url"sv).value_or({})).release_value();
                entry.title = String::from_byte_string(object.get_byte_string("title"sv).value_or({})).release_value();
                if (!entry.url.is_empty())
                    m_recent_history.append(move(entry));
            }
        }
        rebuild_history_menu();
        return;
    }

    if (type == "history_search_results"sv) {
        auto query = String::from_byte_string(event.get_byte_string("query"sv).value_or({})).release_value();
        JsonArray items;
        if (auto values = event.get_array("items"sv); values.has_value())
            items = *values;
        show_history_results(items, query);
        return;
    }

    if (type == "bookmarks"sv) {
        Vector<BookmarkEntry> bookmarks;
        if (auto items = event.get_array("items"sv); items.has_value()) {
            for (auto const& value : items->values()) {
                if (value.is_object())
                    bookmarks.append(bookmark_entry_from_json(value.as_object()));
            }
        }
        m_bookmarks = move(bookmarks);
        rebuild_bookmarks_menu();
        if (m_bookmarks_window)
            rebuild_bookmarks_manager();
        for (auto& tab : m_tabs)
            tab->set_bookmarks(m_bookmarks);
    }
}

void BrowserWindow::handle_engine_tab_event(JsonObject const& event, u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto type = string_for_key(event, "type"sv);

    if (type == "audio"sv) {
        tab->set_audio_state(event.get_bool("playing"sv).value_or(false), event.get_bool("muted"sv).value_or(false));
        update_tab_icon(*tab);
    } else if (type == "title"sv) {
        tab->set_title(String::from_byte_string(string_for_key(event, "title"sv)).release_value());
    } else if (type == "url"sv) {
        tab->set_url(String::from_byte_string(string_for_key(event, "url"sv)).release_value());
        tab->set_bookmarked(event.get_bool("bookmarked"sv).value_or(false));
        if (m_history_refresh_timer)
            m_history_refresh_timer->start();
        if (active_tab() == tab)
            update_bookmark_action();
    } else if (type == "loading"sv) {
        tab->set_loading(event.get_bool("loading"sv).value_or(false));
    } else if (type == "zoom_changed"sv) {
        tab->set_zoom_level(event.get_double_with_precision_loss("zoom"sv).value_or(1.0));
    } else if (type == "autocomplete_results"sv) {
        auto request_id = event.get_u64("request"sv).value_or(0);
        if (auto items = event.get_array("items"sv); items.has_value())
            tab->set_autocomplete_results(request_id, *items, event.get_bool("final"sv).value_or(true));
    } else if (type == "find_result"sv) {
        auto current = event.get_u64("current"sv).value_or(0);
        auto total = event.get_u64("total"sv);
        tab->set_find_result(current, total);
    } else if (type == "context_menu"sv) {
        auto x = event.get_i64("x"sv).value_or(0);
        auto y = event.get_i64("y"sv).value_or(0);
        if (auto items = event.get_array("items"sv); items.has_value())
            tab->show_context_menu(*items, Gfx::IntPoint { static_cast<int>(x), static_cast<int>(y) });
    } else if (type == "permission"sv) {
        auto name = String::from_byte_string(string_for_key(event, "name"sv)).release_value();
        auto origin = String::from_byte_string(string_for_key(event, "origin"sv)).release_value();
        auto request_id = event.get_u64("request"sv).value_or(0);
        bool remembered_decision = false;
        if (!m_private && m_session.remembered_permission(origin, name, remembered_decision)) {
            if (tab->is_attached())
                m_engine->permission_response(tab->tab_id(), request_id, remembered_decision);
        } else {
            tab->show_permission_prompt(name, origin, request_id);
        }
    } else if (type == "certificate_info"sv) {
        tab->show_certificate_info(event);
    } else if (type == "compat_report_ready"sv) {
        tab->show_compat_report(event);
    } else if (type == "compat_report_page_dumps_ready"sv) {
        tab->set_compat_report_page_dumps_ready();
    } else if (type == "compat_report_progress"sv) {
        tab->set_compat_report_progress(String::from_byte_string(string_for_key(event, "stage"sv)).release_value());
    } else if (type == "compat_report_retry"sv) {
        tab->set_compat_report_retry(
            String::from_byte_string(string_for_key(event, "reason"sv)).release_value(),
            static_cast<u32>(event.get_u64("retry"sv).value_or(0)),
            static_cast<u32>(event.get_u64("maximum"sv).value_or(0)),
            static_cast<u32>(event.get_u64("delay"sv).value_or(0)));
    } else if (type == "compat_report_sent"sv) {
        tab->set_compat_report_sent();
    } else if (type == "compat_report_failed"sv) {
        tab->set_compat_report_failed(
            String::from_byte_string(string_for_key(event, "failure"sv)).release_value(),
            String::from_byte_string(string_for_key(event, "reason"sv)).release_value());
    } else if (type == "compat_report_diagnostics"sv) {
        tab->show_compat_report_diagnostics(String::from_byte_string(string_for_key(event, "text"sv)).release_value());
    } else if (type == "input_method_state"sv) {
        Optional<Gfx::IntRect> caret_rect;
        if (auto x = event.get_i64("x"sv); x.has_value()) {
            caret_rect = Gfx::IntRect {
                static_cast<int>(*x),
                static_cast<int>(event.get_i64("y"sv).value_or(0)),
                static_cast<int>(event.get_i64("width"sv).value_or(0)),
                static_cast<int>(event.get_i64("height"sv).value_or(0)),
            };
        }
        tab->update_input_method_state(event.get_bool("enabled"sv).value_or(false), caret_rect);
    } else if (type == "file_picker"sv) {
        auto multiple = event.get_bool("multiple"sv).value_or(false);
        auto filters = event.get_array("filters"sv);
        tab->show_file_picker(multiple, filters.has_value() ? *filters : JsonArray {});
    } else if (type == "select_dropdown"sv) {
        auto x = event.get_i64("x"sv).value_or(0);
        auto y = event.get_i64("y"sv).value_or(0);
        auto minimum_width = static_cast<i32>(event.get_i64("min_width"sv).value_or(0));
        if (auto items = event.get_array("items"sv); items.has_value())
            tab->show_select_dropdown({ static_cast<int>(x), static_cast<int>(y) }, minimum_width, *items);
    } else if (type == "history"sv) {
        tab->set_history(event.get_bool("can_go_back"sv).value_or(false), event.get_bool("can_go_forward"sv).value_or(false));
        if (active_tab() == tab)
            update_action_state();
    } else if (type == "favicon"sv) {
        tab->set_favicon(string_for_key(event, "path"sv));
    } else if (type == "cursor"sv) {
        tab->set_cursor(string_for_key(event, "cursor"sv));
    } else if (type == "link_hover"sv) {
        tab->set_status(string_for_key(event, "url"sv));
    } else if (type == "dialog"sv) {
        tab->show_dialog(string_for_key(event, "kind"sv), string_for_key(event, "message"sv), string_for_key(event, "default"sv));
    } else if (type == "tab_closed"sv) {
        if (tab_is_live(tab))
            remove_tab(*tab);
    } else if (type == "clipboard"sv) {
        auto text = string_for_key(event, "text"sv);
        GUI::Clipboard::the().set_plain_text(text.view());
    } else if (type == "fullscreen"sv) {
        m_window->set_fullscreen(event.get_bool("fullscreen"sv).value_or(false));
    } else if (type == "tab_crashed"sv) {
        tab->set_crashed(true);
    } else if (type == "tab_unresponsive"sv) {
        tab->set_unresponsive(event.get_bool("unresponsive"sv).value_or(true));
    }
}

void BrowserWindow::handle_engine_frame(u64 tab_id, Gfx::Bitmap const* frame, u64 sequence, Gfx::IntRect damage, bool full)
{
    if (auto* tab = tab_for_id(tab_id))
        tab->web_view().set_frame(frame, sequence, damage, full);
}

void BrowserWindow::handle_engine_disconnected()
{
    for (auto& tab : m_tabs)
        tab->set_status("The Ladybird engine disconnected."sv);
}

void BrowserWindow::close_engine_tabs()
{
    for (auto& tab : m_tabs) {
        if (tab->is_attached())
            m_engine->close_tab(tab->tab_id());
        tab->detach();
    }
    if (m_session_window_id != 0)
        m_engine->close_window(m_session_window_id, active_tab_index());
}

Vector<String> BrowserWindow::tab_urls() const
{
    Vector<String> urls;
    for (auto const& tab : m_tabs) {
        auto url = tab->current_url();
        if (url.is_empty())
            url = tab->initial_url();
        urls.append(url.is_empty() ? "about:newtab"_string : move(url));
    }
    return urls;
}

size_t BrowserWindow::active_tab_index() const
{
    auto* active = active_tab();
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].ptr() == active)
            return i;
    }
    return 0;
}

Gfx::IntSize BrowserWindow::window_size() const
{
    return m_window ? m_window->size() : Gfx::IntSize { 1000, 520 };
}

void BrowserWindow::set_window_size(Gfx::IntSize size)
{
    if (m_window)
        m_window->resize(size.width(), size.height());
}

Gfx::IntPoint BrowserWindow::window_position() const
{
    return m_window ? m_window->position() : Gfx::IntPoint {};
}

void BrowserWindow::set_window_position(Gfx::IntPoint position)
{
    if (m_window)
        m_window->move_to(position);
}

bool BrowserWindow::is_maximized() const
{
    return m_window && m_window->is_maximized();
}

void BrowserWindow::set_maximized(bool maximized)
{
    if (m_window)
        m_window->set_maximized(maximized);
}

void BrowserWindow::activate_tab_index(size_t index)
{
    if (index < m_tabs.size())
        m_tab_widget->set_active_widget(m_tabs[index]->root().ptr());
}

void BrowserWindow::handle_download_event(JsonObject const& event)
{
    auto type = string_for_key(event, "type"sv);
    if (type == "download_removed"sv) {
        auto id = event.get_u64("id"sv).value_or(0);
        m_downloads.remove_first_matching([id](auto const& entry) { return entry.id == id; });
    } else if (auto download_object = event.get_object("download"sv); download_object.has_value()) {
        auto entry = download_entry_from_json(*download_object);
        auto index = m_downloads.find_first_index_if([id = entry.id](auto const& candidate) { return candidate.id == id; });
        if (index.has_value())
            m_downloads[*index] = move(entry);
        else
            m_downloads.append(move(entry));
    }

    if (m_downloads_window)
        m_downloads_window->set_downloads(m_downloads);
}

void BrowserWindow::show_downloads()
{
    if (!m_engine)
        return;
    if (!m_downloads_window)
        m_downloads_window = DownloadsWindow::create(*m_engine);
    m_downloads_window->set_downloads(m_downloads);
    m_downloads_window->set_private_filter(m_private);
    m_downloads_window->show();
}

void BrowserWindow::add_bookmark_menu_items(GUI::Menu& menu, Vector<BookmarkEntry> const& items)
{
    for (auto const& item : items) {
        if (item.is_folder) {
            auto submenu = menu.add_submenu(item.title);
            add_bookmark_menu_items(*submenu, item.children);
        } else {
            menu.add_action(GUI::Action::create(item.title.bytes_as_string_view(), [this, url = item.url](auto&) {
                navigate_active(url);
            }));
        }
    }
}

void BrowserWindow::rebuild_bookmarks_menu()
{
    if (!m_bookmarks_menu)
        return;

    m_bookmarks_menu->remove_all_actions();
    if (m_bookmarks.is_empty()) {
        auto empty = GUI::Action::create("No bookmarks"sv, [](auto&) {});
        empty->set_enabled(false);
        m_bookmarks_menu->add_action(empty);
        return;
    }

    add_bookmark_menu_items(*m_bookmarks_menu, m_bookmarks);
}

void BrowserWindow::show_tab_search()
{
    auto dialog = GUI::Dialog::construct(m_window.ptr());
    dialog->set_title("Search Tabs"sv);
    dialog->resize(560, 380);

    auto main_widget = dialog->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

    auto& search_box = main_widget->add<GUI::TextBox>();
    search_box.set_placeholder("Search tabs"sv);
    search_box.set_fixed_height(26);

    Vector<size_t> filtered_indices;
    Vector<GUI::JsonArrayModel::FieldSpec> tab_fields;
    tab_fields.append({ "title"sv, "Title"_string, Gfx::TextAlignment::CenterLeft });
    tab_fields.append({ "url"sv, "Address"_string, Gfx::TextAlignment::CenterLeft });
    auto model = GUI::JsonArrayModel::create_from_source([&]() -> ErrorOr<JsonArray> {
        filtered_indices.clear();

        auto query = search_box.text();
        JsonArray rows;
        for (size_t index = 0; index < m_tabs.size(); ++index) {
            auto& tab = *m_tabs[index];
            auto title = tab.display_title();
            auto url = tab.current_url();

            if (!query.is_empty()
                && !title.contains(query, CaseSensitivity::CaseInsensitive)
                && !url.contains(query, CaseSensitivity::CaseInsensitive)) {
                continue;
            }

            JsonObject row;
            row.set("title"sv, title.bytes_as_string_view());
            row.set("url"sv, url.bytes_as_string_view());
            rows.must_append(move(row));
            filtered_indices.append(index);
        }
        return rows;
    },
        move(tab_fields));

    auto& table = main_widget->add<GUI::TableView>();
    table.set_model(model);
    table.set_column_headers_visible(false);
    table.set_column_width(0, 220);
    table.set_column_width(1, 310);

    Optional<size_t> selected_index;

    auto accept_selection = [&] {
        auto selection = table.selection().first();
        if (!selection.is_valid())
            return;
        auto row = selection.row();
        if (row < 0 || row >= static_cast<int>(filtered_indices.size()))
            return;
        selected_index = filtered_indices[row];
        dialog->done(GUI::Dialog::ExecResult::OK);
    };

    table.on_activation = [&](auto const&) {
        accept_selection();
    };

    search_box.on_change = [&] {
        model->update();
        if (model->row_count() > 0)
            table.set_cursor(model->index(0), GUI::AbstractView::SelectionUpdate::Set);
    };
    search_box.on_return_pressed = [&] {
        accept_selection();
    };
    search_box.on_escape_pressed = [&] {
        dialog->done(GUI::Dialog::ExecResult::Cancel);
    };

    model->update();
    if (model->row_count() > 0)
        table.set_cursor(model->index(0), GUI::AbstractView::SelectionUpdate::Set);
    search_box.set_focus(true);

    if (dialog->exec() != GUI::Dialog::ExecResult::OK || !selected_index.has_value())
        return;

    if (*selected_index < m_tabs.size())
        activate_tab(*m_tabs[*selected_index]);
}

void BrowserWindow::show_tab_context_menu(BrowserTab& tab, Gfx::IntPoint screen_position)
{
    auto* tab_pointer = &tab;
    auto menu = GUI::Menu::construct("Tab"_string);

    menu->add_action(GUI::Action::create("New Tab"sv, [this](auto&) {
        add_tab(String {}, true);
    }));
    menu->add_action(GUI::Action::create("Reload"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            tab_pointer->reload();
    }));
    menu->add_action(GUI::Action::create("Duplicate Tab"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            duplicate_tab(*tab_pointer);
    }));
    menu->add_separator();
    menu->add_action(GUI::Action::create(tab.is_pinned() ? "Unpin Tab"sv : "Pin Tab"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            toggle_pin_tab(*tab_pointer);
    }));
    menu->add_action(GUI::Action::create(tab.is_muted() ? "Unmute Tab"sv : "Mute Tab"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer) && tab_pointer->is_attached())
            m_engine->toggle_mute(tab_pointer->tab_id());
    }));
    menu->add_action(GUI::Action::create("Move Tab to New Window"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            move_tab_to_new_window(*tab_pointer);
    }));
    menu->add_separator();
    menu->add_action(GUI::Action::create("Close Tab"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            close_tab(*tab_pointer);
    }));
    menu->add_action(GUI::Action::create("Close Other Tabs"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            close_other_tabs(*tab_pointer);
    }));
    menu->add_action(GUI::Action::create("Close Tabs to the Right"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            close_tabs_to_the_right(*tab_pointer);
    }));
    menu->add_separator();
    menu->add_action(*m_reopen_closed_tab_action);

    m_tab_context_menu = menu;
    m_tab_context_menu->popup(screen_position);
}

void BrowserWindow::duplicate_tab(BrowserTab& tab)
{
    add_tab(tab.current_url(), true);
}

void BrowserWindow::close_tabs_to_the_right(BrowserTab& tab)
{
    auto index = m_tabs.find_first_index_if([&](auto const& candidate) {
        return candidate.ptr() == &tab;
    });
    if (!index.has_value())
        return;

    Vector<BrowserTab*> to_close;
    for (size_t i = *index + 1; i < m_tabs.size(); ++i)
        to_close.append(m_tabs[i].ptr());
    for (auto* candidate : to_close) {
        if (tab_is_live(candidate))
            close_tab(*candidate);
    }
}

void BrowserWindow::toggle_pin_tab(BrowserTab& tab)
{
    tab.set_pinned(!tab.is_pinned());
    update_tab_icon(tab);
    if (tab.is_attached())
        m_engine->pin_tab(tab.tab_id(), tab.is_pinned());
    rebuild_tab_order();
}

void BrowserWindow::move_tab_to_new_window(BrowserTab& tab)
{
    auto url = tab.current_url();
    if (url.is_empty())
        url = tab.initial_url();
    if (url.is_empty())
        url = "about:newtab"_string;
    m_session.create_window({ url }, m_private);
    close_tab(tab);
}

void BrowserWindow::rebuild_tab_order()
{
    Vector<BrowserTab*> order;
    for (auto& tab : m_tabs) {
        if (tab->is_pinned())
            order.append(tab.ptr());
    }
    for (auto& tab : m_tabs) {
        if (!tab->is_pinned())
            order.append(tab.ptr());
    }

    bool same_order = true;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] != m_tabs[i].ptr()) {
            same_order = false;
            break;
        }
    }
    if (same_order)
        return;

    auto* active = active_tab();
    m_reordering_tabs = true;

    for (auto& tab : m_tabs)
        m_tab_widget->remove_widget(*tab->root());

    Vector<NonnullOwnPtr<BrowserTab>> reordered;
    for (auto* wanted : order) {
        auto index = m_tabs.find_first_index_if([wanted](auto const& candidate) {
            return candidate.ptr() == wanted;
        });
        if (index.has_value())
            reordered.append(m_tabs.take(*index));
    }
    m_tabs = move(reordered);

    for (auto& tab : m_tabs) {
        auto title = tab->display_title();
        if (title.is_empty())
            title = "New Tab"_string;
        m_tab_widget->add_tab(tab->root(), title);
        m_tab_widget->set_tab_icon(*tab->root(), tab_icon_for(*tab));
    }

    if (active && tab_is_live(active))
        m_tab_widget->set_active_widget(active->root().ptr());
    m_reordering_tabs = false;
    rebuild_tabs_menu();
}

Gfx::Bitmap const* BrowserWindow::tab_icon_for(BrowserTab const& tab) const
{
    if (tab.is_muted())
        return m_muted_tab_icon.ptr();
    if (tab.is_audio_playing())
        return m_playing_tab_icon.ptr();
    if (tab.is_pinned())
        return m_pinned_tab_icon.ptr();
    return tab.favicon();
}

void BrowserWindow::update_tab_icon(BrowserTab& tab)
{
    if (m_tab_widget)
        m_tab_widget->set_tab_icon(*tab.root(), tab_icon_for(tab));
}

void BrowserWindow::rebuild_tabs_menu()
{
    if (!m_tabs_menu || m_tabs_menu->is_visible()
        || (m_tabs_popup_menu && m_tabs_popup_menu->is_visible()))
        return;

    auto populate = [this](GUI::Menu& menu) {
        menu.remove_all_actions();
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            auto* tab = m_tabs[i].ptr();
            auto title = tab->display_title();
            if (title.is_empty())
                title = tab->current_url();
            if (title.is_empty())
                title = "New Tab"_string;
            auto label = String::formatted("{}  {}", i + 1, title).release_value();
            menu.add_action(GUI::Action::create(label.bytes_as_string_view(), [this, tab](auto&) {
                if (!tab_is_live(tab))
                    return;
                m_tab_widget->set_active_widget(tab->root().ptr());
                activate_tab(*tab);
            }));
        }
        menu.add_action(*m_new_tab_action);
        menu.add_action(*m_close_tab_action);
    };
    populate(*m_tabs_menu);
    if (m_tabs_popup_menu)
        populate(*m_tabs_popup_menu);

}

void BrowserWindow::rebuild_history_menu()
{
    if (!m_history_menu || m_history_menu->is_visible())
        return;

    m_history_menu->remove_all_actions();
    if (m_recent_history.is_empty()) {
        auto action = GUI::Action::create("No History"sv, [](auto&) {});
        action->set_enabled(false);
        m_history_menu->add_action(action);
    } else {
        for (auto const& entry : m_recent_history) {
            auto title = entry.title.is_empty() ? entry.url : entry.title;
            m_history_menu->add_action(GUI::Action::create(title.bytes_as_string_view(), [this, url = entry.url](auto&) {
                navigate_active(url);
            }));
        }
    }
    m_history_menu->add_action(GUI::Action::create("&Search History…"sv, [this](auto&) {
        show_history_search();
    }));
    m_history_menu->add_action(GUI::Action::create("Clear &History…"sv, [this](auto&) {
        auto result = GUI::MessageBox::show(m_window, "Clear all browsing history?"sv, "Clear History"sv, GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
        if (result == GUI::Dialog::ExecResult::Yes)
            m_engine->clear_history();
    }));
    m_history_menu->add_action(GUI::Action::create("Show &History"sv, [this](auto&) {
        navigate_active("about:history"_string);
    }));

}

void BrowserWindow::show_history_search()
{
    if (!m_engine)
        return;

    String query;
    if (GUI::InputBox::show(m_window, query, "Search history"sv, "Search History"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
        return;
    if (query.is_empty())
        return;
    m_engine->search_history(query, 50);
}

void BrowserWindow::show_history_results(JsonArray const& items, String const& query)
{
    m_history_results_rows.clear();
    m_history_result_urls.clear();

    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;
        auto const& object = value.as_object();
        auto title = String::from_byte_string(object.get_byte_string("title"sv).value_or({})).release_value();
        auto url = String::from_byte_string(object.get_byte_string("url"sv).value_or({})).release_value();
        if (url.is_empty())
            continue;

        JsonObject row;
        row.set("title"sv, (title.is_empty() ? url : title).bytes_as_string_view());
        row.set("url"sv, url.bytes_as_string_view());
        m_history_results_rows.must_append(move(row));
        m_history_result_urls.append(url);
    }

    if (!m_history_results_window) {
        m_history_results_window = GUI::Window::construct();
        m_history_results_window->set_title("History Search"sv);
        m_history_results_window->resize(760, 420);

        auto main_widget = m_history_results_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

        Vector<GUI::JsonArrayModel::FieldSpec> fields;
        fields.append({ "title"sv, "Title"_string, Gfx::TextAlignment::CenterLeft });
        fields.append({ "url"sv, "Address"_string, Gfx::TextAlignment::CenterLeft });
        m_history_results_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
            return m_history_results_rows;
        },
            move(fields));

        m_history_results_table = main_widget->add<GUI::TableView>();
        m_history_results_table->set_model(m_history_results_model);
        m_history_results_table->set_column_headers_visible(true);
        m_history_results_table->set_column_width(0, 320);
        m_history_results_table->set_column_width(1, 380);
        m_history_results_table->on_activation = [this](auto const& index) {
            auto row = index.row();
            if (row >= 0 && row < static_cast<int>(m_history_result_urls.size()))
                navigate_active(m_history_result_urls[row]);
        };

        m_history_results_window->on_close = [this] {
            m_history_results_window->hide();
        };
    }

    m_history_results_window->set_title(ByteString::formatted("History: {}", query.bytes_as_string_view()));
    if (m_history_results_model)
        m_history_results_model->update();
    m_history_results_window->show();
    m_history_results_window->center_on_screen();
}

void BrowserWindow::show_bookmarks_manager()
{
    if (!m_engine)
        return;

    if (!m_bookmarks_window) {
        m_bookmarks_window = GUI::Window::construct();
        m_bookmarks_window->set_title("Bookmarks"sv);
        m_bookmarks_window->resize(780, 360);

        auto main_widget = m_bookmarks_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

        Vector<GUI::JsonArrayModel::FieldSpec> fields;
        fields.append({ "title"sv, "Title"_string, Gfx::TextAlignment::CenterLeft });
        fields.append({ "type"sv, "Type"_string, Gfx::TextAlignment::CenterLeft });
        fields.append({ "url"sv, "Address"_string, Gfx::TextAlignment::CenterLeft });
        m_bookmarks_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
            return m_bookmark_rows;
        },
            move(fields));

        auto& footer = main_widget->add<GUI::Widget>();
        footer.set_fill_with_background_color(true);
        footer.set_fixed_height(28);
        footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 0 }, 4);

        auto& add_bookmark_button = footer.add<GUI::Button>("Add Bookmark"_string);
        add_bookmark_button.set_fixed_width(110);
        add_bookmark_button.on_click = [this](auto) {
            auto* tab = active_tab();
            String title = tab ? tab->display_title() : String {};
            String url = tab ? tab->current_url() : String {};
            if (GUI::InputBox::show(m_bookmarks_window, title, "Bookmark title"sv, "Add Bookmark"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
                return;
            if (GUI::InputBox::show(m_bookmarks_window, url, "Bookmark URL"sv, "Add Bookmark"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
                return;
            if (url.is_empty())
                return;
            m_engine->add_bookmark(tab && tab->is_attached() ? tab->tab_id() : 0, url, title, selected_folder_id());
        };

        auto& add_folder_button = footer.add<GUI::Button>("New Folder"_string);
        add_folder_button.set_fixed_width(90);
        add_folder_button.on_click = [this](auto) {
            String title;
            if (GUI::InputBox::show(m_bookmarks_window, title, "Folder name"sv, "New Folder"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
                return;
            if (title.is_empty())
                return;
            m_engine->add_folder(title, selected_folder_id());
        };

        auto& edit_button = footer.add<GUI::Button>("Edit"_string);
        edit_button.set_fixed_width(60);
        edit_button.on_click = [this](auto) {
            auto const* item = selected_bookmark();
            if (!item)
                return;
            String title = item->title;
            if (GUI::InputBox::show(m_bookmarks_window, title, "Bookmark title"sv, "Edit"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
                return;
            if (item->is_folder) {
                m_engine->edit_folder(item->id, title);
                return;
            }
            String url = item->url;
            if (GUI::InputBox::show(m_bookmarks_window, url, "Bookmark URL"sv, "Edit"sv, GUI::InputType::Text) != GUI::Dialog::ExecResult::OK)
                return;
            if (!url.is_empty())
                m_engine->edit_bookmark(0, item->id, url, title);
        };

        auto& delete_button = footer.add<GUI::Button>("Delete"_string);
        delete_button.set_fixed_width(60);
        delete_button.on_click = [this](auto) {
            if (auto const* item = selected_bookmark())
                m_engine->remove_bookmark(0, item->id);
        };

        auto& up_button = footer.add<GUI::Button>("Move Up"_string);
        up_button.set_fixed_width(70);
        up_button.on_click = [this](auto) {
            auto const* item = selected_bookmark();
            if (!item || item->index == 0)
                return;
            m_engine->move_bookmark(item->id, item->parent, item->index - 1);
        };

        auto& down_button = footer.add<GUI::Button>("Move Down"_string);
        down_button.set_fixed_width(80);
        down_button.on_click = [this](auto) {
            auto const* item = selected_bookmark();
            if (!item || item->index + 1 >= sibling_count(item->parent))
                return;
            m_engine->move_bookmark(item->id, item->parent, item->index + 1);
        };

        auto& close_button = footer.add<GUI::Button>("Close"_string);
        close_button.set_fixed_width(60);
        close_button.on_click = [this](auto) {
            m_bookmarks_window->hide();
        };


        m_bookmarks_table = main_widget->add<GUI::TableView>();
        m_bookmarks_table->set_model(m_bookmarks_model);
        m_bookmarks_table->set_column_headers_visible(true);
        m_bookmarks_table->set_column_width(0, 320);
        m_bookmarks_table->set_column_width(1, 80);
        m_bookmarks_table->set_column_width(2, 360);
        m_bookmarks_table->on_activation = [this](auto const& index) {
            auto row = index.row();
            if (row < 0 || row >= static_cast<int>(m_flat_bookmarks.size()))
                return;
            auto const& item = m_flat_bookmarks[row];
            if (!item.is_folder && !item.url.is_empty())
                navigate_active(item.url);
        };

        m_bookmarks_window->on_close = [this] {
            m_bookmarks_window->hide();
        };
    }

    rebuild_bookmarks_manager();
    m_bookmarks_window->show();
    m_bookmarks_window->center_on_screen();
}

void BrowserWindow::rebuild_bookmarks_manager()
{
    m_flat_bookmarks.clear();
    m_bookmark_rows.clear();

    Function<void(Vector<BookmarkEntry> const&, String const&, size_t)> flatten = [&](Vector<BookmarkEntry> const& items, String const& parent, size_t depth) {
        for (size_t i = 0; i < items.size(); ++i) {
            auto const& item = items[i];
            m_flat_bookmarks.append({ item.id, item.title, item.url, parent, item.is_folder, i, depth });

            StringBuilder indent;
            for (size_t level = 0; level < depth; ++level)
                indent.append("  "sv);
            auto title = item.title.is_empty() ? item.url : item.title;
            auto label = String::formatted("{}{}", indent.string_view(), title).release_value();

            JsonObject row;
            row.set("title"sv, label.bytes_as_string_view());
            row.set("type"sv, item.is_folder ? "Folder"sv : "Bookmark"sv);
            row.set("url"sv, item.url.bytes_as_string_view());
            m_bookmark_rows.must_append(move(row));

            if (item.is_folder)
                flatten(item.children, item.id, depth + 1);
        }
    };
    flatten(m_bookmarks, {}, 0);

    if (m_bookmarks_model)
        m_bookmarks_model->update();
}

BrowserWindow::FlatBookmark const* BrowserWindow::selected_bookmark() const
{
    if (!m_bookmarks_table)
        return nullptr;
    auto const& selection = m_bookmarks_table->selection();
    if (selection.is_empty())
        return nullptr;
    auto row = selection.first().row();
    if (row < 0 || row >= static_cast<int>(m_flat_bookmarks.size()))
        return nullptr;
    return &m_flat_bookmarks[row];
}

String BrowserWindow::selected_folder_id() const
{
    if (auto const* item = selected_bookmark(); item && item->is_folder)
        return item->id;
    return {};
}

size_t BrowserWindow::sibling_count(String const& parent) const
{
    size_t count = 0;
    for (auto const& item : m_flat_bookmarks) {
        if (item.parent == parent)
            ++count;
    }
    return count;
}

String BrowserWindow::bookmark_id_for_url(StringView url) const
{
    if (auto const* bookmark = find_bookmark(m_bookmarks, url))
        return bookmark->id;
    return {};
}

void BrowserWindow::show_bookmark_dialog(bool edit_current)
{
    auto* tab = active_tab();
    if (!tab)
        return;
    auto url = tab->current_url();
    if (url.is_empty())
        return;

    String id;
    String title = tab->display_title();
    if (edit_current) {
        if (auto const* bookmark = find_bookmark(m_bookmarks, url)) {
            id = bookmark->id;
            title = bookmark->title;
        }
        if (id.is_empty())
            return;
    }
    if (title.is_empty())
        title = url;

    auto result = GUI::InputBox::show(m_window, title, edit_current ? "Edit bookmark title"sv : "Bookmark title"sv, edit_current ? "Edit Bookmark"sv : "Add Bookmark"sv, GUI::InputType::Text);
    if (result != GUI::Dialog::ExecResult::OK)
        return;
    if (title.is_empty())
        title = url;

    if (edit_current)
        m_engine->edit_bookmark(tab->tab_id(), id, url, title);
    else
        m_engine->add_bookmark(tab->tab_id(), url, title);
}

void BrowserWindow::close_other_tabs(BrowserTab& keep)
{
    Vector<BrowserTab*> to_close;
    for (auto& tab : m_tabs) {
        if (tab.ptr() != &keep)
            to_close.append(tab.ptr());
    }
    for (auto* tab : to_close) {
        if (tab_is_live(tab))
            close_tab(*tab);
    }
}

void BrowserWindow::show()
{
    m_window->show();
    if (auto* tab = active_tab())
        tab->web_view().set_focus(true);
}

}
