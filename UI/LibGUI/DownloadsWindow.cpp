/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DownloadsWindow.h"

#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Icon.h>
#include <LibGUI/JsonArrayModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/TableView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>

namespace LadybirdSDE {

DownloadsWindow::DownloadsWindow(EngineClient& engine)
    : m_engine(engine)
{
    m_window = GUI::Window::construct();
    m_window->set_title("Downloads"sv);
    m_window->set_icon(GUI::Icon::default_icon("download"sv).bitmap_for_size(32));
    m_window->resize(720, 360);
    m_window->on_close = [this] {
        m_window->hide();
    };

    auto main_widget = m_window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 6 }, 6);

    Vector<GUI::JsonArrayModel::FieldSpec> fields;
    fields.append({ "name"sv, "Name"_string, Gfx::TextAlignment::CenterLeft });
    fields.append({ "status"sv, "Status"_string, Gfx::TextAlignment::CenterLeft });
    fields.append({ "progress"sv, "Progress"_string, Gfx::TextAlignment::CenterRight });
    fields.append({ "size"sv, "Size"_string, Gfx::TextAlignment::CenterRight });

    m_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
        return rows();
    },
        move(fields));

    m_table = main_widget->add<GUI::TableView>();
    m_table->set_model(m_model);
    m_table->set_column_headers_visible(true);
    m_table->set_column_width(0, 260);
    m_table->set_column_width(1, 100);
    m_table->set_column_width(2, 110);
    m_table->set_column_width(3, 110);
    m_table->on_activation = [this](auto&) {
        perform_action_on_selection("open"sv);
    };

    auto& footer = main_widget->add<GUI::Widget>();
    footer.set_fill_with_background_color(true);
    footer.set_fixed_height(28);
    footer.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 0 }, 4);

    auto& open_button = footer.add<GUI::Button>("Open"_string);
    open_button.set_fixed_width(70);
    open_button.on_click = [this](auto) {
        perform_action_on_selection("open"sv);
    };

    auto& show_button = footer.add<GUI::Button>("Show in Folder"_string);
    show_button.set_fixed_width(120);
    show_button.on_click = [this](auto) {
        perform_action_on_selection("show_in_folder"sv);
    };

    auto& pause_button = footer.add<GUI::Button>("Pause / Resume"_string);
    pause_button.set_fixed_width(120);
    pause_button.on_click = [this](auto) {
        pause_or_resume_selection();
    };

    auto& cancel_button = footer.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(70);
    cancel_button.on_click = [this](auto) {
        perform_action_on_selection("cancel"sv);
    };

    auto& clear_button = footer.add<GUI::Button>("Clear Completed"_string);
    clear_button.set_fixed_width(120);
    clear_button.on_click = [this](auto) {
        m_engine.download_action("clear"sv, 0);
    };

    m_summary = footer.add<GUI::Label>(String {});
    m_summary->set_fixed_width(150);
    m_summary->set_text_wrapping(Gfx::TextWrapping::DontWrap);
    m_summary->set_text_alignment(Gfx::TextAlignment::CenterRight);

    refresh();
}

DownloadsWindow::~DownloadsWindow() = default;

NonnullRefPtr<DownloadsWindow> DownloadsWindow::create(EngineClient& engine)
{
    return adopt_ref(*new DownloadsWindow(engine));
}

void DownloadsWindow::show()
{
    refresh();
    m_window->show();
    m_window->center_on_screen();
}

void DownloadsWindow::set_downloads(Vector<DownloadEntry> const& downloads)
{
    m_downloads = downloads;
    refresh();
}

void DownloadsWindow::refresh()
{
    if (m_model)
        m_model->update();

    if (m_summary) {
        auto active = 0;
        for (auto const& download : m_downloads) {
            if (download.status == "in_progress"sv || download.status == "paused"sv)
                ++active;
        }
        m_summary->set_text(String::formatted("{} download{}, {} active", m_downloads.size(), m_downloads.size() == 1 ? "" : "s", active).release_value());
    }
}

JsonArray DownloadsWindow::rows() const
{
    JsonArray rows;
    for (auto const& download : m_downloads) {
        LexicalPath destination { download.destination.bytes_as_string_view() };
        auto basename = destination.basename();
        auto name = basename.is_empty() ? download.url : String::from_utf8(basename).release_value();

        auto status = download.status;
        if (status == "in_progress"sv)
            status = "In progress"_string;
        else if (status == "paused"sv)
            status = "Paused"_string;
        else if (status == "completed"sv)
            status = "Completed"_string;
        else if (status == "canceled"sv)
            status = "Canceled"_string;
        else if (status == "failed"sv)
            status = "Failed"_string;

        String progress;
        if (download.status == "completed"sv)
            progress = "100%"_string;
        else if (download.progress.has_value())
            progress = String::formatted("{}%", static_cast<int>(*download.progress * 100)).release_value();
        else if (download.total.has_value() && *download.total > 0)
            progress = String::formatted("{}%", static_cast<int>((download.downloaded * 100) / *download.total)).release_value();

        String size = human_readable_size(download.downloaded);
        if (download.total.has_value())
            size = String::formatted("{} / {}", size, human_readable_size(*download.total)).release_value();

        JsonObject row;
        row.set("name"sv, name.bytes_as_string_view());
        row.set("status"sv, status.bytes_as_string_view());
        row.set("progress"sv, progress.bytes_as_string_view());
        row.set("size"sv, size.bytes_as_string_view());
        rows.must_append(move(row));
    }
    return rows;
}

DownloadEntry const* DownloadsWindow::selected_download() const
{
    auto const& selection = m_table->selection();
    if (selection.is_empty())
        return nullptr;

    auto row = selection.first().row();
    if (row < 0 || row >= static_cast<int>(m_downloads.size()))
        return nullptr;
    return &m_downloads[row];
}

void DownloadsWindow::perform_action_on_selection(StringView action)
{
    if (auto* download = selected_download())
        m_engine.download_action(action, download->id);
}

void DownloadsWindow::pause_or_resume_selection()
{
    auto* download = selected_download();
    if (!download)
        return;
    if (download->status == "paused"sv)
        m_engine.download_action("resume"sv, download->id);
    else if (download->status == "in_progress"sv)
        m_engine.download_action("pause"sv, download->id);
}

}
