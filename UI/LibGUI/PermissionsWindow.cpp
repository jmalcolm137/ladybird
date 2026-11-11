/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PermissionsWindow.h"

#include "BrowserSession.h"

#include <AK/JsonObject.h>
#include <AK/StringBuilder.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/JsonArrayModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>

namespace LadybirdSDE {

PermissionsWindow::PermissionsWindow(BrowserSession& session)
    : m_session(session)
{
    m_window = GUI::Window::construct();
    m_window->set_title("Permissions"sv);
    m_window->resize(720, 360);
    m_window->on_close = [this] {
        m_window->hide();
    };

    auto main_widget = m_window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

    Vector<GUI::JsonArrayModel::FieldSpec> fields;
    fields.append({ "origin"sv, "Website"_string, Gfx::TextAlignment::CenterLeft });
    fields.append({ "permissions"sv, "Permissions"_string, Gfx::TextAlignment::CenterLeft });

    m_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
        return rows();
    },
        move(fields));

    m_table = main_widget->add<GUI::TableView>();
    m_table->set_model(m_model);
    m_table->set_column_headers_visible(true);
    m_table->set_column_width(0, 300);
    m_table->set_column_width(1, 390);

    auto& footer = main_widget->add<GUI::Widget>();
    footer.set_fill_with_background_color(true);
    footer.set_fixed_height(28);
    footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 0 }, 4);

    auto& revoke_button = footer.add<GUI::Button>("Revoke"_string);
    revoke_button.set_fixed_width(80);
    revoke_button.on_click = [this](auto) {
        revoke_selected();
    };

    auto& clear_button = footer.add<GUI::Button>("Clear All"_string);
    clear_button.set_fixed_width(90);
    clear_button.on_click = [this](auto) {
        m_session.clear_all_permissions();
        refresh();
    };

    footer.add_spacer();

    m_summary = footer.add<GUI::Label>(String {});
    m_summary->set_fixed_width(220);
    m_summary->set_text_wrapping(Gfx::TextWrapping::DontWrap);
    m_summary->set_text_alignment(Gfx::TextAlignment::CenterRight);

    refresh();
}

PermissionsWindow::~PermissionsWindow() = default;

NonnullRefPtr<PermissionsWindow> PermissionsWindow::create(BrowserSession& session)
{
    return adopt_ref(*new PermissionsWindow(session));
}

void PermissionsWindow::show()
{
    refresh();
    m_window->show();
    m_window->center_on_screen();
}

void PermissionsWindow::refresh()
{
    if (m_model)
        m_model->update();

    if (m_summary) {
        if (m_origins.is_empty())
            m_summary->set_text("No saved site permissions"_string);
        else
            m_summary->set_text(String::formatted("{} saved site{}", m_origins.size(), m_origins.size() == 1 ? "" : "s").release_value());
    }
}

JsonArray PermissionsWindow::rows()
{
    m_origins.clear();
    JsonArray rows;

    for (auto const& permission : m_session.origin_permissions()) {
        m_origins.append(permission.origin);
        JsonObject row;
        row.set("origin"sv, permission.origin.bytes_as_string_view());
        row.set("permissions"sv, permission.summary.bytes_as_string_view());
        rows.must_append(move(row));
    }

    return rows;
}

int PermissionsWindow::selected_row() const
{
    if (!m_table)
        return -1;
    auto const& selection = m_table->selection();
    if (selection.is_empty())
        return -1;
    return selection.first().row();
}

void PermissionsWindow::revoke_selected()
{
    auto row = selected_row();
    if (row < 0 || row >= static_cast<int>(m_origins.size()))
        return;

    m_session.forget_permissions(m_origins[row]);
    refresh();
}

}
