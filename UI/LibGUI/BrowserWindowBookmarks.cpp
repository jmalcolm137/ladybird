/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserWindow.h"
#include "BrowserSession.h"
#include "ChromeUtils.h"

#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <LibCore/Environment.h>
#include <LibCore/Timer.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/JsonArrayModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TableView.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>

namespace LadybirdSDE {

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


void BrowserWindow::add_bookmark_menu_items(GUI::Menu& menu, Vector<BookmarkEntry> const& items)
{
    for (auto const& item : items) {
        auto safe_title = escape_menu_ampersands(item.title.bytes_as_string_view());
        if (item.is_folder) {
            auto submenu = menu.add_submenu(safe_title);
            add_bookmark_menu_items(*submenu, item.children);
        } else {
            menu.add_action(GUI::Action::create(safe_title.bytes_as_string_view(), [this, url = item.url](auto&) {
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
            auto title = escape_menu_ampersands((entry.title.is_empty() ? entry.url : entry.title).bytes_as_string_view());
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

    auto tab_id = tab->tab_id();
    auto weak_this = make_weak_ptr();
    auto result = GUI::InputBox::show(m_window, title, edit_current ? "Edit bookmark title"sv : "Bookmark title"sv, edit_current ? "Edit Bookmark"sv : "Add Bookmark"sv, GUI::InputType::Text);
    if (!weak_this || result != GUI::Dialog::ExecResult::OK)
        return;
    if (title.is_empty())
        title = url;

    if (edit_current)
        m_engine->edit_bookmark(tab_id, id, url, title);
    else
        m_engine->add_bookmark(tab_id, url, title);
}


}
