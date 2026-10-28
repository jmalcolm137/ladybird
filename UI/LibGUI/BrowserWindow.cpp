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
#include <LibGUI/Clipboard.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
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

BrowserWindow::BrowserWindow(BrowserSession& session, EngineClient& engine, bool private_window)
    : m_session(session)
    , m_engine(&engine)
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

ErrorOr<void> BrowserWindow::initialize(Vector<String> initial_urls)
{
    m_window = GUI::Window::construct();
    m_window->set_title(m_private ? "Ladybird (Private)"sv : "Ladybird"sv);
    m_window->resize(1000, 520);
    m_window->set_icon(GUI::Icon::default_icon("app-browser"sv).bitmap_for_size(32));

    m_tab_widget = m_window->set_main_widget<GUI::TabWidget>();
    m_tab_widget->set_container_margins(GUI::Margins {});
    m_tab_widget->set_uniform_tabs(true);
    m_tab_widget->set_close_button_enabled(true);
    m_tab_widget->set_text_alignment(Gfx::TextAlignment::CenterLeft);
    m_tab_widget->set_reorder_allowed(true);

    create_actions();
    create_menus();

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

    create_initial_tabs(initial_urls);
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

    m_next_tab_action = GUI::Action::create("&Next Tab"sv, GUI::Shortcut(Mod_Ctrl, Key_PageDown), [this](auto&) {
        m_tab_widget->activate_next_tab();
    }, m_window.ptr());
    m_previous_tab_action = GUI::Action::create("&Previous Tab"sv, GUI::Shortcut(static_cast<u8>(Mod_Ctrl | Mod_Shift), Key_PageUp), [this](auto&) {
        m_tab_widget->activate_previous_tab();
    }, m_window.ptr());

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
        auto data = GUI::Clipboard::the().fetch_data_and_type();
        if (!data.mime_type.starts_with("text/"sv))
            return;
        auto text = StringView { reinterpret_cast<char const*>(data.data.data()), data.data.size() };
        m_engine->paste(tab->tab_id(), text);
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

    m_restore_session_action = GUI::Action::create("Restore Previous &Session"sv, [this](auto&) {
        m_session.restore_previous_session();
    }, m_window.ptr());
    m_restore_session_action->set_status_tip("Reopen the tabs from the previous session"_string);

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
    file_menu.add_separator();
    file_menu.add_action(*m_next_tab_action);
    file_menu.add_action(*m_previous_tab_action);
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
    bookmarks_menu.add_action(GUI::Action::create("Show &Bookmarks"sv, [this](auto&) {
        navigate_active("about:bookmarks"_string);
    }, m_window.ptr()));

    auto& history_menu = *m_window->add_menu("&History"_string);
    history_menu.add_action(GUI::Action::create("Show &History"sv, [this](auto&) {
        navigate_active("about:history"_string);
    }, m_window.ptr()));
    history_menu.add_separator();
    history_menu.add_action(*m_restore_session_action);

    auto& tools_menu = *m_window->add_menu("&Tools"_string);
    tools_menu.add_action(GUI::Action::create("&Settings"sv, [this](auto&) {
        navigate_active("about:settings"_string);
    }, m_window.ptr()));
    tools_menu.add_action(*m_downloads_action);

    auto& help_menu = *m_window->add_menu("&Help"_string);
    help_menu.add_action(GUI::CommonActions::make_about_action("Ladybird"_string, GUI::Icon::default_icon("app-browser"sv), m_window.ptr()));
}

void BrowserWindow::create_initial_tabs(Vector<String> const& initial_urls)
{
    if (initial_urls.is_empty()) {
        add_tab(String {}, true);
        return;
    }

    for (size_t i = 0; i < initial_urls.size(); ++i)
        add_tab(initial_urls[i], i + 1 == initial_urls.size());
}

BrowserTab& BrowserWindow::add_tab(String const& url, bool activate, bool request_engine_tab)
{
    auto tab = make<BrowserTab>(*m_engine, *m_window, *m_back_action, *m_forward_action, *m_reload_action, *m_stop_action, *m_bookmark_action, *m_home_action, *m_new_tab_action, *m_downloads_action);
    auto* tab_pointer = tab.ptr();
    tab->set_initial_url(url);

    tab->on_title_change = [this, tab_pointer](String const& title) {
        if (!tab_is_live(tab_pointer))
            return;
        m_tab_widget->set_tab_title(*tab_pointer->root(), title);
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
    tab->on_favicon_change = [this, tab_pointer](Gfx::Bitmap const* icon) {
        if (!tab_is_live(tab_pointer))
            return;
        m_tab_widget->set_tab_icon(*tab_pointer->root(), icon);
    };
    tab->on_loading_change = [this, tab_pointer](bool) {
        if (!tab_is_live(tab_pointer))
            return;
        if (active_tab() == tab_pointer)
            update_window_title();
    };

    m_tabs.append(move(tab));
    tab_pointer->set_bookmarks(m_bookmarks);
    tab_pointer->set_bookmarks_bar_visible(m_show_bookmarks_bar);
    m_tab_widget->add_tab(tab_pointer->root(), "New Tab"_string);

    if (request_engine_tab) {
        if (m_engine->is_connected())
            tab_pointer->create_engine_tab(m_session.next_request_id(), url, m_private);
        else
            tab_pointer->set_status("Starting the Ladybird engine..."sv);
    }

    if (activate)
        m_tab_widget->set_active_widget(tab_pointer->root().ptr());

    update_action_state();
    update_window_title();
    return *tab_pointer;
}

void BrowserWindow::close_tab(BrowserTab& tab)
{
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
    if (tab.is_attached())
        m_engine->activate_tab(tab.tab_id());
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
    m_bookmark_action->set_enabled(tab != nullptr);
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
    for (auto& tab : m_tabs) {
        if (!tab->is_attached()) {
            tab->create_engine_tab(m_session.next_request_id(), tab->initial_url(), m_private);
            tab->set_status(""sv);
        }
    }
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

    if (type == "bookmarks"sv) {
        Vector<BookmarkEntry> bookmarks;
        if (auto items = event.get_array("items"sv); items.has_value()) {
            for (auto const& value : items->values()) {
                if (!value.is_object())
                    continue;
                auto const& object = value.as_object();
                BookmarkEntry entry;
                entry.title = String::from_byte_string(string_for_key(object, "title"sv)).release_value();
                entry.url = String::from_byte_string(string_for_key(object, "url"sv)).release_value();
                bookmarks.append(move(entry));
            }
        }
        m_bookmarks = move(bookmarks);
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

    if (type == "title"sv) {
        tab->set_title(String::from_byte_string(string_for_key(event, "title"sv)).release_value());
    } else if (type == "url"sv) {
        tab->set_url(String::from_byte_string(string_for_key(event, "url"sv)).release_value());
        tab->set_bookmarked(event.get_bool("bookmarked"sv).value_or(false));
        if (active_tab() == tab)
            update_bookmark_action();
    } else if (type == "loading"sv) {
        tab->set_loading(event.get_bool("loading"sv).value_or(false));
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
        tab->show_permission_prompt(string_for_key(event, "name"sv), string_for_key(event, "origin"sv), event.get_u64("request"sv).value_or(0));
    } else if (type == "file_picker"sv) {
        tab->show_file_picker({});
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
    }
}

void BrowserWindow::handle_engine_frame(u64 tab_id, Gfx::Bitmap const* frame)
{
    if (auto* tab = tab_for_id(tab_id))
        tab->web_view().set_frame(frame);
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
}

Vector<String> BrowserWindow::tab_urls() const
{
    Vector<String> urls;
    for (auto const& tab : m_tabs) {
        auto url = tab->current_url();
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
    m_downloads_window->show();
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
    menu->add_action(GUI::Action::create("Close Tab"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            close_tab(*tab_pointer);
    }));
    menu->add_action(GUI::Action::create("Close Other Tabs"sv, [this, tab_pointer](auto&) {
        if (tab_is_live(tab_pointer))
            close_other_tabs(*tab_pointer);
    }));

    m_tab_context_menu = menu;
    m_tab_context_menu->popup(screen_position);
}

void BrowserWindow::duplicate_tab(BrowserTab& tab)
{
    add_tab(tab.current_url(), true);
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
