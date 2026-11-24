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

static String format_memory(u64 bytes)
{
    if (bytes >= 1ull << 30)
        return String::formatted("{:.1f} GiB", static_cast<double>(bytes) / static_cast<double>(1ull << 30)).release_value();
    if (bytes >= 1ull << 20)
        return String::formatted("{:.1f} MiB", static_cast<double>(bytes) / static_cast<double>(1ull << 20)).release_value();
    if (bytes >= 1ull << 10)
        return String::formatted("{:.1f} KiB", static_cast<double>(bytes) / static_cast<double>(1ull << 10)).release_value();
    return String::formatted("{} B", bytes).release_value();
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

void BrowserWindow::show_user_agent_dialog()
{
    auto dialog = GUI::Dialog::construct(m_window.ptr());
    dialog->set_title("User Agent"sv);
    dialog->resize(640, 210);

    auto main_widget = dialog->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10 }, 8);

    auto& description = main_widget->add<GUI::Label>("Choose a preset or enter a custom User-Agent string."_string);
    description.set_text_wrapping(Gfx::TextWrapping::Wrap);

    Vector<String> names;
    for (auto const& preset : m_session.user_agent_presets())
        names.append(preset.name);
    names.append("Custom…"_string);

    auto& preset_combo = main_widget->add<GUI::ComboBox>();
    preset_combo.set_model(GUI::ItemListModel<String>::create(names));
    preset_combo.set_only_allow_values_from_model(true);

    auto& user_agent_box = main_widget->add<GUI::TextBox>();
    user_agent_box.set_fixed_height(26);
    user_agent_box.set_text(m_session.current_user_agent().bytes_as_string_view());

    size_t selected_index = names.size() - 1;
    for (size_t index = 0; index < m_session.user_agent_presets().size(); ++index) {
        if (m_session.user_agent_presets()[index].value == m_session.current_user_agent()) {
            selected_index = index;
            break;
        }
    }
    preset_combo.set_selected_index(selected_index);

    preset_combo.on_change = [&](ByteString const&, GUI::ModelIndex const& index) {
        if (!index.is_valid() || index.row() >= static_cast<int>(m_session.user_agent_presets().size()))
            return;
        user_agent_box.set_text(m_session.user_agent_presets()[index.row()].value.bytes_as_string_view());
    };

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>();
    buttons.set_fixed_height(26);
    auto& spacer = buttons.add<GUI::Widget>();
    spacer.set_preferred_width(GUI::SpecialDimension::Grow);

    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(80);
    cancel_button.on_click = [&](auto) {
        dialog->done(GUI::Dialog::ExecResult::Cancel);
    };

    auto& apply_button = buttons.add<GUI::Button>("Apply"_string);
    apply_button.set_fixed_width(80);
    apply_button.on_click = [&](auto) {
        auto value = String::from_byte_string(user_agent_box.text()).release_value().trim_ascii_whitespace().release_value();
        m_session.set_user_agent(move(value));
        dialog->done(GUI::Dialog::ExecResult::OK);
    };

    dialog->exec();
}

void BrowserWindow::show_navigator_compatibility_dialog()
{
    auto dialog = GUI::Dialog::construct(m_window.ptr());
    dialog->set_title("Navigator Compatibility Mode"sv);
    dialog->resize(560, 170);

    auto main_widget = dialog->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10 }, 8);

    auto& description = main_widget->add<GUI::Label>("Choose how navigator.appVersion, platform, vendor, and productSub are reported."_string);
    description.set_text_wrapping(Gfx::TextWrapping::Wrap);

    Vector<String> names;
    for (auto const& mode : m_session.navigator_compatibility_modes())
        names.append(mode.name);

    auto& mode_combo = main_widget->add<GUI::ComboBox>();
    mode_combo.set_model(GUI::ItemListModel<String>::create(names));
    mode_combo.set_only_allow_values_from_model(true);

    size_t selected_index = 0;
    for (size_t index = 0; index < m_session.navigator_compatibility_modes().size(); ++index) {
        if (m_session.navigator_compatibility_modes()[index].value == m_session.current_navigator_compatibility_mode()) {
            selected_index = index;
            break;
        }
    }
    if (!names.is_empty())
        mode_combo.set_selected_index(selected_index);

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>();
    buttons.set_fixed_height(26);
    auto& spacer = buttons.add<GUI::Widget>();
    spacer.set_preferred_width(GUI::SpecialDimension::Grow);

    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(80);
    cancel_button.on_click = [&](auto) {
        dialog->done(GUI::Dialog::ExecResult::Cancel);
    };

    auto& apply_button = buttons.add<GUI::Button>("Apply"_string);
    apply_button.set_fixed_width(80);
    apply_button.on_click = [&](auto) {
        auto index = mode_combo.selected_index();
        if (index < m_session.navigator_compatibility_modes().size())
            m_session.set_navigator_compatibility_mode(m_session.navigator_compatibility_modes()[index].value);
        dialog->done(GUI::Dialog::ExecResult::OK);
    };
    apply_button.set_enabled(!names.is_empty());

    dialog->exec();
}

void BrowserWindow::show_crash_report_notification(String const& report_name)
{
    m_crash_prompt_name = report_name;

    if (!m_crash_prompt_window) {
        m_crash_prompt_window = GUI::Window::construct();
        m_crash_prompt_window->set_title("Crash Report"sv);
        m_crash_prompt_window->resize(480, 160);

        auto main_widget = m_crash_prompt_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12 }, 8);

        m_crash_prompt_label = main_widget->add<GUI::Label>("Part of Ladybird stopped unexpectedly. You can send us a crash report to help fix it."_string);
        m_crash_prompt_label->set_text_wrapping(Gfx::TextWrapping::Wrap);

        auto& buttons = main_widget->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>();
        buttons.set_fixed_height(26);
        auto& spacer = buttons.add<GUI::Widget>();
        spacer.set_preferred_width(GUI::SpecialDimension::Grow);

        auto& dismiss_button = buttons.add<GUI::Button>("Dismiss"_string);
        dismiss_button.set_fixed_width(80);
        dismiss_button.on_click = [this](auto) {
            if (m_crash_prompt_window)
                m_crash_prompt_window->hide();
            m_engine->crash_report_dismiss();
        };

        auto& review_button = buttons.add<GUI::Button>("Review Report"_string);
        review_button.set_fixed_width(120);
        review_button.on_click = [this](auto) {
            if (m_crash_prompt_window)
                m_crash_prompt_window->hide();
            if (!m_crash_prompt_name.is_empty())
                m_engine->crash_report_open(m_crash_prompt_name);
        };

        m_crash_prompt_window->on_close = [this] {
            if (m_crash_prompt_window)
                m_crash_prompt_window->hide();
            m_engine->crash_report_dismiss();
        };
    }

    m_crash_prompt_label->set_text(String::formatted("Part of Ladybird stopped unexpectedly (report {}). You can send us a crash report to help fix it.", report_name).release_value());
    m_crash_prompt_window->show();
    m_crash_prompt_window->move_to_front();
}

void BrowserWindow::show_crash_report(JsonObject const& event)
{
    if (!m_crash_report_window) {
        m_crash_report_window = GUI::Window::construct();
        m_crash_report_window->set_title("Crash Report"sv);
        m_crash_report_window->resize(760, 540);
        m_crash_report_window->on_close = [this] {
            if (m_crash_report_window)
                m_crash_report_window->hide();
            m_engine->crash_report_close();
        };

        auto main_widget = m_crash_report_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 10 }, 6);

        m_crash_report_title = main_widget->add<GUI::Label>("Part of Ladybird stopped unexpectedly"_string);
        m_crash_report_title->set_text_wrapping(Gfx::TextWrapping::Wrap);

        m_crash_report_fields = main_widget->add<GUI::Widget>();
        m_crash_report_fields->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 0 }, 1);

        auto& description_label = main_widget->add<GUI::Label>("What were you doing when it stopped?"_string);
        description_label.set_text_wrapping(Gfx::TextWrapping::Wrap);

        m_crash_report_description = main_widget->add<GUI::TextEditor>();
        m_crash_report_description->set_mode(GUI::TextEditor::Editable);
        m_crash_report_description->set_wrapping_mode(GUI::TextEditor::WrapAtWords);
        m_crash_report_description->set_fixed_height(84);
        m_crash_report_description->set_placeholder("Describe what happened, if you can."_string);

        auto& website_row = main_widget->add<GUI::Widget>();
        website_row.set_layout<GUI::HorizontalBoxLayout>();
        website_row.set_fixed_height(26);
        m_crash_report_include_website = website_row.add<GUI::CheckBox>("Include website"_string);
        m_crash_report_include_website->set_fixed_width(140);
        m_crash_report_website = website_row.add<GUI::TextBox>();
        m_crash_report_website->set_preferred_width(GUI::SpecialDimension::Grow);
        m_crash_report_website->set_enabled(false);
        m_crash_report_include_website->on_checked = [this](bool checked) {
            if (m_crash_report_website)
                m_crash_report_website->set_enabled(checked);
        };

        m_crash_report_status = main_widget->add<GUI::Label>("Review the report, add a description, then send."_string);
        m_crash_report_status->set_text_wrapping(Gfx::TextWrapping::Wrap);

        auto& buttons = main_widget->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>();
        buttons.set_fixed_height(26);

        auto& show_button = buttons.add<GUI::Button>("Show Report…"_string);
        show_button.set_fixed_width(110);
        show_button.on_click = [this](auto) {
            m_engine->crash_report_text();
        };

        m_crash_report_next_button = buttons.add<GUI::Button>("Next Report"_string);
        m_crash_report_next_button->set_fixed_width(100);
        m_crash_report_next_button->on_click = [this](auto) {
            m_engine->crash_report_next();
        };
        m_crash_report_next_button->set_visible(false);

        auto& spacer = buttons.add<GUI::Widget>();
        spacer.set_preferred_width(GUI::SpecialDimension::Grow);

        auto& close_button = buttons.add<GUI::Button>("Close"_string);
        close_button.set_fixed_width(80);
        close_button.on_click = [this](auto) {
            if (m_crash_report_window)
                m_crash_report_window->hide();
            m_engine->crash_report_close();
        };

        m_crash_report_send_button = buttons.add<GUI::Button>("Send Report"_string);
        m_crash_report_send_button->set_fixed_width(110);
        m_crash_report_send_button->on_click = [this](auto) {
            auto description = String::from_byte_string(m_crash_report_description->text()).release_value();
            auto url = String::from_byte_string(m_crash_report_website->text()).release_value();
            m_engine->crash_report_send(description, m_crash_report_include_website->is_checked(), url);
            if (m_crash_report_status)
                m_crash_report_status->set_text("Sending crash report…"_string);
            if (m_crash_report_send_button)
                m_crash_report_send_button->set_enabled(false);
        };
    }

    auto name = String::from_byte_string(string_for_key(event, "name"sv)).release_value();
    m_crash_report_name = name;
    if (m_crash_report_title)
        m_crash_report_title->set_text(String::formatted("Part of Ladybird stopped unexpectedly (report {}).", name).release_value());

    if (m_crash_report_fields) {
        m_crash_report_fields->remove_all_children();
        if (auto fields = event.get_array("fields"sv); fields.has_value()) {
            for (auto const& value : fields->values()) {
                if (!value.is_object())
                    continue;
                auto const& field = value.as_object();
                auto label = String::from_byte_string(field.get_byte_string("label"sv).value_or({})).release_value();
                auto detail = String::from_byte_string(field.get_byte_string("value"sv).value_or({})).release_value();
                auto& text = m_crash_report_fields->add<GUI::Label>(String::formatted("{}: {}", label, detail).release_value());
                text.set_text_wrapping(Gfx::TextWrapping::Wrap);
                text.set_text_alignment(Gfx::TextAlignment::CenterLeft);
                text.set_fixed_height(18);
            }
        }
        m_crash_report_fields->invalidate_layout();
    }

    if (m_crash_report_description)
        m_crash_report_description->set_text({});
    if (m_crash_report_include_website)
        m_crash_report_include_website->set_checked(false);
    if (m_crash_report_website) {
        auto url = active_tab() ? active_tab()->current_url() : String {};
        m_crash_report_website->set_text(url.bytes_as_string_view());
        m_crash_report_website->set_enabled(false);
    }
    if (m_crash_report_status)
        m_crash_report_status->set_text("Review the report, add a description, then send."_string);
    if (m_crash_report_send_button)
        m_crash_report_send_button->set_enabled(true);
    if (m_crash_report_next_button)
        m_crash_report_next_button->set_visible(event.get_bool("has_next"sv).value_or(false));

    m_crash_report_window->show();
    m_crash_report_window->move_to_front();
}

void BrowserWindow::show_crash_report_text(String const& text)
{
    if (!m_crash_report_text_window) {
        m_crash_report_text_window = GUI::Window::construct();
        m_crash_report_text_window->set_title("Crash Report"sv);
        m_crash_report_text_window->resize(780, 520);
        m_crash_report_text_window->on_close = [this] {
            if (m_crash_report_text_window)
                m_crash_report_text_window->hide();
        };

        auto main_widget = m_crash_report_text_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);
        m_crash_report_text = main_widget->add<GUI::TextEditor>();
        m_crash_report_text->set_mode(GUI::TextEditor::ReadOnly);
        m_crash_report_text->set_wrapping_mode(GUI::TextEditor::NoWrap);
    }

    m_crash_report_text->set_text(text.bytes_as_string_view());
    m_crash_report_text_window->show();
    m_crash_report_text_window->move_to_front();
}

void BrowserWindow::show_process_manager()
{
    if (!m_process_window) {
        m_process_window = GUI::Window::construct();
        m_process_window->set_title("Process Manager"sv);
        m_process_window->resize(860, 480);
        m_process_window->on_close = [this] {
            if (m_process_timer)
                m_process_timer->stop();
            if (m_process_window)
                m_process_window->hide();
        };

        auto main_widget = m_process_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8 }, 6);

        Vector<GUI::JsonArrayModel::FieldSpec> fields;
        fields.append({ "name"sv, "Process"_string, Gfx::TextAlignment::CenterLeft });
        fields.append({ "pid"sv, "PID"_string, Gfx::TextAlignment::CenterRight });
        fields.append({ "cpu"sv, "CPU"_string, Gfx::TextAlignment::CenterRight });
        fields.append({ "memory"sv, "Memory"_string, Gfx::TextAlignment::CenterRight });
        fields.append({ "page"sv, "Page"_string, Gfx::TextAlignment::CenterLeft });
        m_process_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
            return m_process_rows;
        },
            move(fields));

        m_process_table = main_widget->add<GUI::TableView>();
        m_process_table->set_model(m_process_model);
        m_process_table->set_column_width(0, 260);
        m_process_table->set_column_width(1, 80);
        m_process_table->set_column_width(2, 80);
        m_process_table->set_column_width(3, 100);
        m_process_table->set_column_width(4, 300);

        auto& buttons = main_widget->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>();
        buttons.set_fixed_height(26);

        auto& refresh_button = buttons.add<GUI::Button>("Refresh"_string);
        refresh_button.set_fixed_width(80);
        refresh_button.on_click = [this](auto) {
            m_engine->request_processes();
        };

        auto& end_button = buttons.add<GUI::Button>("End Process"_string);
        end_button.set_fixed_width(100);
        end_button.on_click = [this](auto) {
            auto selection = m_process_table->selection().first();
            if (!selection.is_valid() || selection.row() < 0 || selection.row() >= static_cast<int>(m_process_rows.size()))
                return;
            auto const& row = m_process_rows.at(selection.row());
            if (!row.is_object())
                return;
            if (row.as_object().get_byte_string("type"sv).value_or({}) == "Browser"sv) {
                if (m_process_status)
                    m_process_status->set_text("The browser process cannot be ended from here."_string);
                return;
            }
            if (auto child = row.as_object().get_u64("child"sv); child.has_value())
                m_engine->end_process(*child);
        };

        m_process_status = buttons.add<GUI::Label>(""_string);
        m_process_status->set_preferred_width(GUI::SpecialDimension::Grow);

        m_process_timer = Core::Timer::create_repeating(1000, [this] {
            m_engine->request_processes();
        });
        m_process_timer->start();
    }

    if (m_process_timer)
        m_process_timer->start();
    m_engine->request_processes();
    m_process_window->show();
    m_process_window->move_to_front();
}

void BrowserWindow::update_processes(JsonObject const& event)
{
    JsonArray rows;
    if (auto items = event.get_array("items"sv); items.has_value()) {
        for (auto const& value : items->values()) {
            if (!value.is_object())
                continue;
            auto const& object = value.as_object();
            auto type = String::from_byte_string(object.get_byte_string("type"sv).value_or({})).release_value();
            auto name = type;
            if (auto title = object.get_byte_string("title"sv); title.has_value() && !title->is_empty()) {
                if (type != "WebContent"sv)
                    name = String::formatted("{} — {}", type, *title).release_value();
            }

            auto page = String::from_byte_string(object.get_byte_string("page"sv).value_or({})).release_value();
            if (page.is_empty())
                page = String::from_byte_string(object.get_byte_string("site"sv).value_or({})).release_value();

            auto cpu = object.get_double_with_precision_loss("cpu"sv).value_or(0.0);
            auto memory = object.get_u64("memory"sv).value_or(0);

            auto cpu_text = String::formatted("{:.1f}%", cpu).release_value();
            auto memory_text = format_memory(memory);
            JsonObject row;
            row.set("type"sv, type.bytes_as_string_view());
            row.set("name"sv, name.bytes_as_string_view());
            row.set("pid"sv, object.get_u64("pid"sv).value_or(0));
            row.set("cpu"sv, cpu_text.bytes_as_string_view());
            row.set("memory"sv, memory_text.bytes_as_string_view());
            row.set("page"sv, page.bytes_as_string_view());
            row.set("child"sv, object.get_u64("child"sv).value_or(0));
            rows.must_append(move(row));
        }
    }

    m_process_rows = move(rows);
    if (m_process_model)
        m_process_model->update();
    if (Core::Environment::has("LADYBIRD_SDE_TEST_LOG_PROCESSES"sv))
        warnln("process manager: {} processes", m_process_rows.size());
    if (m_process_status) {
        auto total_cpu = event.get_double_with_precision_loss("total_cpu"sv).value_or(0.0);
        auto total_memory = event.get_u64("total_memory"sv).value_or(0);
        m_process_status->set_text(String::formatted("{} processes · {:.1f}% CPU · {}", m_process_rows.size(), total_cpu, format_memory(total_memory)).release_value());
    }
}


}
