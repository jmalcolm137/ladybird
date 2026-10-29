/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserTab.h"

#include <LibGUI/BoxLayout.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/ToolbarContainer.h>
#include <LibGfx/Bitmap.h>

namespace LadybirdSDE {

BrowserTab::BrowserTab(EngineClient& engine, GUI::Window& window, GUI::Action& back_action, GUI::Action& forward_action, GUI::Action& reload_action, GUI::Action& stop_action, GUI::Action& bookmark_action, GUI::Action& home_action, GUI::Action& new_tab_action, GUI::Action& downloads_action)
    : m_engine(engine)
    , m_root(GUI::Widget::construct())
{
    m_title = "New Tab"_string;

    m_root->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 0 }, 0);

    auto& toolbar_container = m_root->add<GUI::ToolbarContainer>();
    m_toolbar = toolbar_container.add<GUI::Toolbar>();

    m_toolbar->add_action(back_action);
    m_toolbar->add_action(forward_action);
    m_toolbar->add_action(reload_action);
    m_toolbar->add_action(stop_action);
    m_toolbar->add_action(bookmark_action);
    m_toolbar->add_action(home_action);
    m_toolbar->add_action(new_tab_action);
    m_toolbar->add_action(downloads_action);
    m_toolbar->add_separator();

    m_bookmarks_bar = m_root->add<GUI::Widget>();
    m_bookmarks_bar->set_fixed_height(28);
    m_bookmarks_bar->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 4 }, 4);
    m_bookmarks_bar->set_visible(false);

    m_location_box = m_toolbar->add<GUI::TextBox>();
    m_location_box->set_placeholder("Search or enter address"sv);
    m_location_box->set_preferred_width(GUI::SpecialDimension::Grow);
    m_location_box->set_fixed_height(24);
    m_location_box->on_return_pressed = [this] {
        auto text = String::from_byte_string(m_location_box->text()).release_value();
        if (text.is_empty())
            return;
        navigate(text);
        m_web_view->set_focus(true);
    };

    m_web_view = m_root->add<WebContentView>();
    m_web_view->set_preferred_size(GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow);

    m_find_bar = m_root->add<GUI::Widget>();
    m_find_bar->set_fixed_height(30);
    m_find_bar->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 4 }, 4);
    m_find_bar->set_visible(false);

    m_find_bar->add<GUI::Label>("Find:"_string);
    m_find_box = m_find_bar->add<GUI::TextBox>();
    m_find_box->set_preferred_width(GUI::SpecialDimension::Grow);
    m_find_box->on_change = [this] {
        if (is_attached())
            m_engine.find_in_page(m_tab_id, String::from_byte_string(m_find_box->text()).release_value());
    };
    m_find_box->on_return_pressed = [this] {
        if (is_attached())
            m_engine.find_next(m_tab_id);
    };

    auto& previous_button = m_find_bar->add<GUI::Button>("Previous"_string);
    previous_button.on_click = [this](auto) {
        if (is_attached())
            m_engine.find_previous(m_tab_id);
    };

    auto& next_button = m_find_bar->add<GUI::Button>("Next"_string);
    next_button.on_click = [this](auto) {
        if (is_attached())
            m_engine.find_next(m_tab_id);
    };

    m_find_status = m_find_bar->add<GUI::Label>(String {});
    m_find_status->set_fixed_width(80);
    m_find_status->set_text_alignment(Gfx::TextAlignment::CenterRight);

    auto& close_button = m_find_bar->add<GUI::Button>("Close"_string);
    close_button.on_click = [this](auto) {
        hide_find_bar();
    };

    m_statusbar = m_root->add<GUI::Statusbar>();
    m_statusbar->set_fixed_height(22);
    m_statusbar->set_text(String {});

    m_web_view->on_resize = [this](Gfx::IntSize size) {
        if (is_attached())
            m_engine.resize(m_tab_id, size);
    };
    m_web_view->on_mouse = [this](StringView type, Gfx::IntPoint position, GUI::MouseButton button, unsigned buttons, unsigned modifiers, int click_count) {
        if (!is_attached())
            return;
        m_engine.mouse_event(m_tab_id, type, position, StringView { button_name(button) }, buttons, modifiers, click_count);
    };
    m_web_view->on_wheel = [this](Gfx::IntPoint position, int delta_x, int delta_y, unsigned modifiers) {
        if (is_attached())
            m_engine.wheel_event(m_tab_id, position, delta_x, delta_y, modifiers);
    };
    m_web_view->on_key = [this](StringView type, StringView key_name, u32 code_point, unsigned modifiers) {
        if (is_attached())
            m_engine.key_event(m_tab_id, type, key_name, code_point, modifiers);
    };
    m_web_view->on_focus_change = [this](bool focused) {
        if (is_attached())
            m_engine.set_focus(m_tab_id, focused);
    };
    m_web_view->on_go_back = [this] { go_back(); };
    m_web_view->on_go_forward = [this] { go_forward(); };
    m_web_view->on_reload = [this] { reload(); };
    m_web_view->on_force_reload = [this] {
        if (on_hard_reload)
            on_hard_reload();
    };
    m_web_view->on_frame_ack = [this](u64 sequence) {
        if (is_attached())
            m_engine.frame_ack(m_tab_id, sequence);
    };

    // Keep the window association explicit; the tab itself lives in a TabWidget.
    (void)window;
}

StringView BrowserTab::button_name(GUI::MouseButton button)
{
    switch (button) {
    case GUI::MouseButton::Primary:
        return "left"sv;
    case GUI::MouseButton::Secondary:
        return "right"sv;
    case GUI::MouseButton::Middle:
        return "middle"sv;
    case GUI::MouseButton::Backward:
        return "back"sv;
    case GUI::MouseButton::Forward:
        return "forward"sv;
    default:
        return "none"sv;
    }
}

Gfx::IntSize BrowserTab::content_size() const
{
    auto size = m_web_view->size();
    if (size.is_empty())
        return { 1024, 768 };
    return size;
}

void BrowserTab::create_engine_tab(u64 request_id, String const& url, bool private_tab)
{
    m_request_id = request_id;
    m_initial_url = url;
    m_private = private_tab;
    m_engine.create_tab(content_size(), url, request_id, private_tab);
}

void BrowserTab::attach_tab_id(u64 tab_id)
{
    m_tab_id = tab_id;
    m_engine.activate_tab(tab_id);
    m_engine.resize(tab_id, content_size());
    m_engine.set_focus(tab_id, m_web_view->is_focused());
}

void BrowserTab::detach()
{
    m_tab_id = 0;
}

void BrowserTab::navigate(String const& url)
{
    if (is_attached())
        m_engine.navigate(m_tab_id, url, true);
}

void BrowserTab::reload()
{
    if (is_attached())
        m_engine.reload(m_tab_id);
}

void BrowserTab::stop()
{
    if (is_attached())
        m_engine.stop(m_tab_id);
}

void BrowserTab::go_back()
{
    if (is_attached())
        m_engine.go_back(m_tab_id);
}

void BrowserTab::go_forward()
{
    if (is_attached())
        m_engine.go_forward(m_tab_id);
}

void BrowserTab::focus_location()
{
    m_location_box->set_focus(true);
    m_location_box->select_all();
}

void BrowserTab::show_find_bar()
{
    m_find_bar->set_visible(true);
    m_find_box->set_focus(true);
    m_find_box->select_all();
}

void BrowserTab::hide_find_bar()
{
    m_find_bar->set_visible(false);
    if (is_attached())
        m_engine.find_in_page(m_tab_id, String {});
    m_web_view->set_focus(true);
}

void BrowserTab::set_bookmarks(Vector<BookmarkEntry> const& bookmarks)
{
    Vector<NonnullRefPtr<GUI::Widget>> children;
    m_bookmarks_bar->for_each_child_widget([&](GUI::Widget& child) {
        children.append(child);
        return IterationDecision::Continue;
    });
    for (auto& child : children)
        m_bookmarks_bar->remove_child(*child);

    for (auto const& bookmark : bookmarks) {
        auto& button = m_bookmarks_bar->add<GUI::Button>(bookmark.title);
        button.set_button_style(Gfx::ButtonStyle::Coolbar);
        button.set_fixed_height(22);
        button.set_fixed_width(static_cast<int>(bookmark.title.bytes_as_string_view().length()) * 7 + 24);
        button.set_tooltip(bookmark.url);
        button.on_click = [this, url = bookmark.url](auto) {
            navigate(url);
        };
    }

    m_has_bookmarks = !bookmarks.is_empty();
    m_bookmarks_bar->set_visible(m_bookmarks_bar_visible && m_has_bookmarks);
}

void BrowserTab::set_bookmarks_bar_visible(bool visible)
{
    m_bookmarks_bar_visible = visible;
    m_bookmarks_bar->set_visible(visible && m_has_bookmarks);
}

void BrowserTab::set_bookmarked(bool bookmarked)
{
    if (m_bookmarked == bookmarked)
        return;
    m_bookmarked = bookmarked;
    if (on_bookmark_change)
        on_bookmark_change(bookmarked);
}

void BrowserTab::set_loading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    if (loading)
        m_web_view->set_crashed(false);
    m_statusbar->set_text(loading ? "Loading..."_string : String {});
    if (on_loading_change)
        on_loading_change(loading);
}

void BrowserTab::set_history(bool can_go_back, bool can_go_forward)
{
    m_can_go_back = can_go_back;
    m_can_go_forward = can_go_forward;
}

void BrowserTab::set_url(String const& url)
{
    if (m_location_box->text() == url.to_byte_string())
        return;
    m_location_box->set_text(url.bytes_as_string_view());
    if (on_url_change)
        on_url_change(url);
}

void BrowserTab::set_title(String const& title)
{
    m_title = title.is_empty() ? "Untitled"_string : title;
    if (on_title_change)
        on_title_change(m_title);
}

void BrowserTab::set_favicon(StringView path)
{
    if (path.is_empty()) {
        if (on_favicon_change)
            on_favicon_change(nullptr);
        return;
    }

    auto bitmap = Gfx::Bitmap::load_from_file(path, 1, Gfx::IntSize { 16, 16 });
    if (bitmap.is_error())
        return;
    if (on_favicon_change)
        on_favicon_change(bitmap.value().ptr());
}

void BrowserTab::set_cursor(StringView name)
{
    m_web_view->set_page_cursor(name);
}

void BrowserTab::set_status(StringView text)
{
    m_statusbar->set_override_text(text.is_empty() ? Optional<String> {} : String::from_utf8(text).release_value());
}


String BrowserTab::current_url() const
{
    return String::from_byte_string(m_location_box->text()).release_value();
}

void BrowserTab::set_crashed(bool crashed)
{
    m_web_view->set_crashed(crashed);
    if (crashed)
        set_status("The page crashed. Click the page to reload."sv);
}

void BrowserTab::set_unresponsive(bool unresponsive)
{
    m_web_view->set_unresponsive(unresponsive);
    set_status(unresponsive ? "The page is not responding. Click the banner to reload."sv : ""sv);
}

void BrowserTab::set_find_result(u64 current, Optional<u64> total)
{
    if (!m_find_status)
        return;
    if (!total.has_value() || *total == 0) {
        m_find_status->set_text("No matches"_string);
        return;
    }
    m_find_status->set_text(String::formatted("{}/{}", current, *total).release_value());
}

void BrowserTab::add_context_menu_items(GUI::Menu& menu, JsonArray const& items)
{
    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        if (object.get_bool("separator"sv).value_or(false)) {
            menu.add_separator();
            continue;
        }

        auto title = String::from_byte_string(object.get_byte_string("title"sv).value_or({})).release_value();
        if (auto submenu_items = object.get_array("items"sv); submenu_items.has_value()) {
            auto submenu = menu.add_submenu(title);
            add_context_menu_items(*submenu, *submenu_items);
            continue;
        }

        auto text = object.get_byte_string("text"sv).value_or({});
        auto action_index = object.get_u64("action"sv);
        if (!action_index.has_value())
            continue;

        auto checkable = object.get_bool("checkable"sv).value_or(false);
        auto enabled = object.get_bool("enabled"sv).value_or(true);

        NonnullRefPtr<GUI::Action> action = checkable
            ? GUI::Action::create_checkable(text, [this, index = *action_index](auto&) {
                  if (is_attached())
                      m_engine.context_menu_action(m_tab_id, index);
              })
            : GUI::Action::create(text, [this, index = *action_index](auto&) {
                  if (is_attached())
                      m_engine.context_menu_action(m_tab_id, index);
              });
        if (checkable)
            action->set_checked(object.get_bool("checked"sv).value_or(false));

        action->set_enabled(enabled);
        menu.add_action(action);
    }
}

void BrowserTab::show_context_menu(JsonArray const& items, Gfx::IntPoint view_position)
{
    auto menu = GUI::Menu::construct("Context Menu"_string);
    add_context_menu_items(*menu, items);

    auto root_origin = m_root->screen_relative_rect().location();
    auto view_origin = m_web_view->screen_relative_rect().location();
    auto popup_position = view_origin.translated(-root_origin.x(), -root_origin.y()).translated(view_position.x(), view_position.y());

    m_context_menu = menu;
    m_context_menu->popup(popup_position);
}

void BrowserTab::show_permission_prompt(StringView name, StringView origin, u64 request_id)
{
    auto message = String::formatted("Allow {} for {}?", name, origin).release_value();
    auto result = GUI::MessageBox::show(m_root->window(), message, "Permission Request"sv,
        GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
    if (is_attached())
        m_engine.permission_response(m_tab_id, request_id, result == GUI::Dialog::ExecResult::Yes);
}

void BrowserTab::show_file_picker(StringView)
{
    auto path = GUI::FilePicker::get_open_filepath(m_root->window(), "Select File"sv);
    if (!is_attached())
        return;

    if (path.has_value())
        m_engine.file_picker_response(m_tab_id, String::from_byte_string(path.release_value()).release_value());
    else
        m_engine.file_picker_response(m_tab_id, String {});
}

void BrowserTab::add_select_dropdown_items(GUI::Menu& menu, JsonArray const& items, bool in_group)
{
    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        if (object.get_bool("separator"sv).value_or(false)) {
            menu.add_separator();
            continue;
        }

        if (auto group_items = object.get_array("items"sv); group_items.has_value()) {
            auto group_label = String::from_byte_string(object.get_byte_string("group"sv).value_or({})).release_value();
            auto group_action = GUI::Action::create(group_label.to_byte_string(), [](auto&) {});
            group_action->set_enabled(false);
            menu.add_action(group_action);
            add_select_dropdown_items(menu, *group_items, true);
            continue;
        }

        auto label = String::from_byte_string(object.get_byte_string("label"sv).value_or({})).release_value();
        if (in_group)
            label = String::formatted("    {}", label).release_value();
        auto id = object.get_u64("id"sv);
        if (!id.has_value())
            continue;

        auto action = GUI::Action::create_checkable(label.to_byte_string(), [this, item_id = static_cast<u32>(*id)](auto&) {
            m_selected_select_item = item_id;
        });
        action->set_checked(object.get_bool("selected"sv).value_or(false));
        action->set_enabled(!object.get_bool("disabled"sv).value_or(false));
        menu.add_action(action);
    }
}

void BrowserTab::show_select_dropdown(Gfx::IntPoint position, i32 minimum_width, JsonArray const& items)
{
    auto menu = GUI::Menu::construct("Select Dropdown"_string);
    menu->set_minimum_width(minimum_width);
    add_select_dropdown_items(*menu, items);

    m_selected_select_item.clear();
    m_select_dropdown_visible = false;
    menu->on_visibility_change = [this](bool visible) {
        if (visible) {
            m_select_dropdown_visible = true;
            return;
        }
        if (!m_select_dropdown_visible)
            return;

        m_select_dropdown_visible = false;
        auto selected = m_selected_select_item;
        m_selected_select_item.clear();
        if (is_attached())
            m_engine.select_dropdown_closed(m_tab_id, selected);
    };

    m_select_dropdown = menu;
    auto root_origin = m_root->screen_relative_rect().location();
    auto view_origin = m_web_view->screen_relative_rect().location();
    auto popup_position = view_origin.translated(-root_origin.x(), -root_origin.y()).translated(position.x(), position.y());
    m_select_dropdown->popup(popup_position);
}

void BrowserTab::show_dialog(StringView kind, StringView message, StringView default_text)
{
    auto& window = *m_root->window();
    auto text = String::from_utf8(message).release_value();

    if (kind == "alert"sv) {
        GUI::MessageBox::show(&window, text, "Alert"sv, GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK);
        m_engine.dialog_response(m_tab_id, "alert"sv, true);
        return;
    }

    if (kind == "confirm"sv) {
        auto result = GUI::MessageBox::show(&window, text, "Confirm"sv, GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
        m_engine.dialog_response(m_tab_id, "confirm"sv, result == GUI::Dialog::ExecResult::Yes);
        return;
    }

    if (kind == "prompt"sv) {
        auto value = String::from_utf8(default_text).release_value();
        auto result = GUI::InputBox::show(&window, value, text, "Prompt"sv, GUI::InputType::Text);
        m_engine.dialog_response(m_tab_id, "prompt"sv, result == GUI::Dialog::ExecResult::OK, value);
    }
}

}
