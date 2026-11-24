/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserTab.h"
#include "ChromeUtils.h"

#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Dialog.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Label.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibURL/URL.h>

namespace LadybirdSDE {

static String host_for_dialog_title(String const& url)
{
    auto parsed = URL::URL(url.to_byte_string());
    if (!parsed.is_valid())
        return {};
    if (auto host = parsed.serialized_host(); !host.is_error())
        return host.release_value();
    return {};
}

static String truncate_dialog_text(StringView text, size_t max_bytes)
{
    if (text.length() <= max_bytes)
        return String::from_utf8_with_replacement_character(text);
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<u8>(text[cut]) & 0xC0) == 0x80)
        --cut;
    StringBuilder builder;
    builder.append(text.substring_view(0, cut));
    builder.append("…"sv);
    return MUST(builder.to_string());
}

class PermissionPrompt final : public GUI::Dialog {
public:
    static Optional<bool> run(GUI::Window* parent, String const& message, bool& remember)
    {
        auto dialog = adopt_ref(*new PermissionPrompt(parent, message));
        auto result = dialog->exec();
        remember = dialog->m_remember->is_checked();
        if (result == GUI::Dialog::ExecResult::Yes)
            return true;
        if (result == GUI::Dialog::ExecResult::No)
            return false;
        return {};
    }

private:
    PermissionPrompt(GUI::Window* parent, String const& message)
        : Dialog(parent)
    {
        set_title("Permission Request"sv);
        set_resizable(false);
        set_auto_shrink(true);

        auto main_widget = set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12 }, 8);

        auto& label = main_widget->add<GUI::Label>(message);
        label.set_text_wrapping(Gfx::TextWrapping::DontWrap);
        label.set_autosize(true);

        m_remember = main_widget->add<GUI::CheckBox>("Remember this decision for this site"_string);

        auto& buttons = main_widget->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 0 }, 8);
        buttons.set_fixed_height(24);
        buttons.add_spacer();

        auto& block = buttons.add<GUI::Button>("Block"_string);
        block.set_fixed_width(70);
        block.on_click = [this](auto) { done(GUI::Dialog::ExecResult::No); };

        auto& allow = buttons.add<GUI::Button>("Allow"_string);
        allow.set_fixed_width(70);
        allow.on_click = [this](auto) { done(GUI::Dialog::ExecResult::Yes); };
    }

    RefPtr<GUI::CheckBox> m_remember;
};


void BrowserTab::show_certificate_info(JsonObject const& event)
{
    auto get_string = [&](StringView key) -> String {
        auto value = event.get_byte_string(key);
        if (!value.has_value())
            return {};
        return String::from_byte_string(*value).release_value();
    };

    if (!event.get_bool("ok"sv).value_or(false)) {
        auto error = get_string("error"sv);
        if (error.is_empty())
            error = "Certificate details are unavailable."_string;
        GUI::MessageBox::show(m_root->window(), error, "Certificate"sv, GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK);
        return;
    }

    auto host = host_for_dialog_title(current_url());
    StringBuilder builder;
    builder.appendff("Certificate for {}\n\n", host.is_empty() ? "this site"sv : host.bytes_as_string_view());
    auto append_field = [&](StringView name, String const& value) {
        builder.appendff("{}\n    {}\n\n", name, value.is_empty() ? "Unknown"sv : value.bytes_as_string_view());
    };
    if (auto verified = event.get_bool("verified"sv); verified.has_value()) {
        if (*verified) {
            append_field("Verification"sv, "Verified"_string);
        } else {
            auto reason = get_string("verification_error"sv);
            append_field("Verification"sv, reason.is_empty() ? "Not verified"_string : String::formatted("Not verified ({})", reason).release_value());
        }
    }
    append_field("Subject"sv, get_string("subject"sv));
    append_field("Issuer"sv, get_string("issuer"sv));
    append_field("Valid from"sv, get_string("valid_from"sv));
    append_field("Valid until"sv, get_string("valid_until"sv));
    append_field("Serial number"sv, get_string("serial_number"sv));
    append_field("SHA-256 fingerprint"sv, get_string("sha256_fingerprint"sv));
    append_field("Protocol"sv, get_string("protocol"sv));
    append_field("Cipher"sv, get_string("cipher"sv));

    if (!m_certificate_window) {
        m_certificate_window = GUI::Window::construct();
        m_certificate_window->set_title("Certificate"sv);
        m_certificate_window->resize(640, 420);
        m_certificate_window->on_close = [this] {
            m_certificate_window->hide();
        };

        auto main_widget = m_certificate_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8 }, 6);

        m_certificate_text = main_widget->add<GUI::TextEditor>();
        m_certificate_text->set_mode(GUI::TextEditor::ReadOnly);
        m_certificate_text->set_wrapping_mode(GUI::TextEditor::WrapAtWords);
    }

    m_certificate_text->set_text(builder.string_view());
    m_certificate_window->set_title(ByteString::formatted("Certificate - {}", host.bytes_as_string_view()));
    m_certificate_window->show();
    m_certificate_window->center_on_screen();
}

void BrowserTab::show_compat_report(JsonObject const& event)
{
    auto get_string = [&](StringView key) -> String {
        auto value = event.get_byte_string(key);
        if (!value.has_value())
            return {};
        return String::from_byte_string(*value).release_value();
    };

    auto url = get_string("url"sv);
    m_compat_report_page_dumps_ready = event.get_bool("has_page_dumps"sv).value_or(false);
    m_compat_report_has_screenshot = event.get_bool("has_screenshot"sv).value_or(false);

    if (!m_compat_report_window) {
        m_compat_report_window = GUI::Window::construct();
        m_compat_report_window->set_title("Report Website Issue"sv);
        m_compat_report_window->resize(720, 380);
        m_compat_report_window->on_close = [this] {
            if (is_attached())
                m_engine.compat_report_close(m_tab_id);
            m_compat_report_window->hide();
        };

        auto main_widget = m_compat_report_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12 }, 8);

        m_compat_report_website = main_widget->add<GUI::Label>(String {});
        m_compat_report_website->set_text_wrapping(Gfx::TextWrapping::Wrap);
        m_compat_report_website->set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& prompt = main_widget->add<GUI::Label>("Describe what is wrong with this website:"_string);
        prompt.set_autosize(false);
        prompt.set_fixed_height(18);
        prompt.set_preferred_width(GUI::SpecialDimension::Grow);
        prompt.set_text_alignment(Gfx::TextAlignment::CenterLeft);

        m_compat_report_description = main_widget->add<GUI::TextEditor>();
        m_compat_report_description->set_mode(GUI::TextEditor::Editable);
        m_compat_report_description->set_wrapping_mode(GUI::TextEditor::WrapAtWords);
        m_compat_report_description->set_fixed_height(80);
        m_compat_report_description->set_placeholder("What did you expect, and what happened instead?"sv);

        m_compat_report_include_screenshot = main_widget->add<GUI::CheckBox>("Include a screenshot of the page"_string);
        m_compat_report_include_dumps = main_widget->add<GUI::CheckBox>("Include page layout and paint dumps"_string);

        m_compat_report_status = main_widget->add<GUI::Label>("Ready to send."_string);
        m_compat_report_status->set_text_wrapping(Gfx::TextWrapping::Wrap);
        m_compat_report_status->set_text_alignment(Gfx::TextAlignment::CenterLeft);

        auto& buttons = main_widget->add<GUI::Widget>();
        buttons.set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 0 }, 8);
        buttons.set_fixed_height(28);
        buttons.add_spacer();

        auto& show_report = buttons.add<GUI::Button>("Show Report…"_string);
        show_report.set_fixed_width(110);
        show_report.on_click = [this](auto) {
            if (is_attached())
                m_engine.compat_report_diagnostics(m_tab_id, m_compat_report_include_dumps->is_checked());
        };

        m_compat_report_retry_button = buttons.add<GUI::Button>("Retry"_string);
        m_compat_report_retry_button->set_fixed_width(70);
        m_compat_report_retry_button->set_visible(false);
        m_compat_report_retry_button->on_click = [this](auto) {
            m_compat_report_retry_button->set_visible(false);
            m_compat_report_send_button->set_enabled(false);
            m_compat_report_status->set_text("Preparing to retry…"_string);
            if (is_attached())
                m_engine.compat_report_retry(m_tab_id);
        };

        m_compat_report_send_button = buttons.add<GUI::Button>("Send"_string);
        m_compat_report_send_button->set_fixed_width(70);
        m_compat_report_send_button->on_click = [this](auto) {
            auto description = String::from_byte_string(m_compat_report_description->text()).release_value();
            auto trimmed = description.trim_ascii_whitespace();
            if (trimmed.is_error() || trimmed.value().is_empty()) {
                m_compat_report_status->set_text("Describe what is wrong with this website before sending."_string);
                m_compat_report_description->set_focus(true);
                return;
            }
            m_compat_report_send_button->set_enabled(false);
            m_compat_report_retry_button->set_visible(false);
            m_compat_report_status->set_text("Preparing report…"_string);
            if (is_attached())
                m_engine.compat_report_send(m_tab_id, description, m_compat_report_include_dumps->is_checked(), m_compat_report_include_screenshot->is_checked());
        };

        auto& close_button = buttons.add<GUI::Button>("Close"_string);
        close_button.set_fixed_width(70);
        close_button.on_click = [this](auto) {
            if (is_attached())
                m_engine.compat_report_close(m_tab_id);
            m_compat_report_window->hide();
        };
    }

    auto host = host_for_dialog_title(url);
    m_compat_report_website->set_text(String::formatted("Website: {}", url).release_value());
    m_compat_report_window->set_title(ByteString::formatted("Report Website Issue - {}", host.is_empty() ? "page"sv : host.bytes_as_string_view()));

    m_compat_report_description->set_text({});
    m_compat_report_include_screenshot->set_checked(m_compat_report_has_screenshot);
    m_compat_report_include_screenshot->set_enabled(m_compat_report_has_screenshot);
    m_compat_report_include_dumps->set_checked(m_compat_report_page_dumps_ready);
    m_compat_report_include_dumps->set_enabled(m_compat_report_page_dumps_ready);
    m_compat_report_status->set_text(m_compat_report_page_dumps_ready ? "Ready to send."_string : "Preparing the page dumps…"_string);
    m_compat_report_send_button->set_enabled(true);
    m_compat_report_retry_button->set_visible(false);

    m_compat_report_window->show();
    m_compat_report_window->center_on_screen();
    m_compat_report_description->set_focus(true);
}

void BrowserTab::set_compat_report_page_dumps_ready()
{
    m_compat_report_page_dumps_ready = true;
    if (!m_compat_report_window)
        return;
    m_compat_report_include_dumps->set_enabled(true);
    m_compat_report_include_dumps->set_checked(true);
    m_compat_report_status->set_text("Ready to send."_string);
}

void BrowserTab::set_compat_report_progress(String const& stage)
{
    if (!m_compat_report_window)
        return;
    m_compat_report_status->set_text(String::formatted("{}…", stage).release_value());
}

void BrowserTab::set_compat_report_retry(String const& reason, u32 retry, u32 maximum, u32 delay)
{
    if (!m_compat_report_window)
        return;
    m_compat_report_status->set_text(String::formatted("Sending failed: {}. Retrying in {}s ({}/{}).", reason, delay, retry, maximum).release_value());
}

void BrowserTab::set_compat_report_sent()
{
    if (!m_compat_report_window)
        return;
    m_compat_report_status->set_text("Report sent. Thank you!"_string);
    m_compat_report_send_button->set_enabled(false);
    m_compat_report_retry_button->set_visible(false);
}

void BrowserTab::set_compat_report_failed(String const& failure, String const& reason)
{
    if (!m_compat_report_window)
        return;
    m_compat_report_status->set_text(String::formatted("Could not send the report: {}", reason).release_value());
    if (failure == "validation"sv) {
        m_compat_report_send_button->set_enabled(true);
        m_compat_report_retry_button->set_visible(false);
        return;
    }
    m_compat_report_send_button->set_enabled(false);
    m_compat_report_retry_button->set_visible(true);
}

void BrowserTab::show_compat_report_diagnostics(String const& text)
{
    if (!m_compat_report_diagnostics_window) {
        m_compat_report_diagnostics_window = GUI::Window::construct();
        m_compat_report_diagnostics_window->set_title("Compatibility Report"sv);
        m_compat_report_diagnostics_window->resize(760, 520);
        m_compat_report_diagnostics_window->on_close = [this] {
            m_compat_report_diagnostics_window->hide();
        };

        auto main_widget = m_compat_report_diagnostics_window->set_main_widget<GUI::Widget>();
        main_widget->set_fill_with_background_color(true);
        main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8 }, 6);

        m_compat_report_diagnostics_text = main_widget->add<GUI::TextEditor>();
        m_compat_report_diagnostics_text->set_mode(GUI::TextEditor::ReadOnly);
        m_compat_report_diagnostics_text->set_wrapping_mode(GUI::TextEditor::NoWrap);
    }

    m_compat_report_diagnostics_text->set_text(text);
    m_compat_report_diagnostics_window->show();
    m_compat_report_diagnostics_window->center_on_screen();
}


void BrowserTab::show_permission_prompt(StringView name, StringView origin, u64 request_id)
{
    auto name_copy = String::from_utf8_with_replacement_character(name);
    auto origin_copy = String::from_utf8_with_replacement_character(origin);
    auto weak_this = make_weak_ptr();
    Core::deferred_invoke([weak_this, name = move(name_copy), origin = move(origin_copy), request_id]() mutable {
        auto* tab = weak_this.ptr();
        if (!tab || !tab->is_attached())
            return;
        auto message = String::formatted("Allow {} for {}?", name, origin).release_value();
        bool remember = false;
        auto decision = PermissionPrompt::run(tab->m_root->window(), message, remember);
        tab = weak_this.ptr();
        if (!tab)
            return;
        if (!decision.has_value()) {
            if (tab->is_attached())
                tab->m_engine.permission_response(tab->m_tab_id, request_id, false, false);
            return;
        }

        if (tab->on_permission_decision)
            tab->on_permission_decision(origin, name, *decision, remember);
        if (tab->is_attached())
            tab->m_engine.permission_response(tab->m_tab_id, request_id, *decision, remember && !tab->m_private);
    });
}


void BrowserTab::show_file_picker(bool, JsonArray const& filters)
{
    auto weak_this = make_weak_ptr();
    Core::deferred_invoke([weak_this, filters]() mutable {
        auto* tab = weak_this.ptr();
        if (!tab || !tab->is_attached())
            return;
        // LibGUI's file picker exposes one path at a time, so a multiple-file
        // request still opens a single selection. The engine accepts a list, so the
        // dialog can become multi-select without changing the protocol.
        Optional<Vector<GUI::FileTypeFilter>> allowed_filters;
        Vector<GUI::FileTypeFilter> parsed_filters;

        for (auto const& value : filters.values()) {
            if (!value.is_string())
                continue;
            auto filter = String::from_utf8_with_replacement_character(value.as_string());
            auto view = filter.bytes_as_string_view();
            if (view == "Image files"sv)
                parsed_filters.append(GUI::FileTypeFilter::image_files());
            else if (view == "Audio files"sv)
                parsed_filters.append(GUI::FileTypeFilter::audio_files());
            else if (view == "Video files"sv || view.starts_with("video/"sv))
                parsed_filters.append(GUI::FileTypeFilter::video_files());
            else if (view.starts_with("audio/"sv))
                parsed_filters.append(GUI::FileTypeFilter::audio_files());
            else if (view.starts_with("image/"sv))
                parsed_filters.append(GUI::FileTypeFilter::image_files());
            else if (view.starts_with("*."sv))
                parsed_filters.append({ "Files"sv, Vector<ByteString> { filter.bytes_as_string_view().substring_view(2) } });
        }
        if (!parsed_filters.is_empty())
            allowed_filters = move(parsed_filters);

        auto path = GUI::FilePicker::get_open_filepath(tab->m_root->window(), "Select File"sv, Core::StandardPaths::home_directory(), false, GUI::Dialog::ScreenPosition::CenterWithinParent, allowed_filters);
        tab = weak_this.ptr();
        if (!tab || !tab->is_attached())
            return;

        if (path.has_value())
            tab->m_engine.file_picker_response(tab->m_tab_id, String::from_utf8_with_replacement_character(path.release_value()));
        else
            tab->m_engine.file_picker_response(tab->m_tab_id, String {});
    });
}


void BrowserTab::show_dialog(StringView kind, StringView message, StringView default_text)
{
    static constexpr size_t max_dialog_message_bytes = 2048;
    static constexpr size_t max_dialog_default_bytes = 1024;

    auto kind_copy = String::from_utf8_with_replacement_character(kind);
    auto text_copy = truncate_dialog_text(message, max_dialog_message_bytes);
    auto default_copy = truncate_dialog_text(default_text, max_dialog_default_bytes);
    auto weak_this = make_weak_ptr();
    Core::deferred_invoke([weak_this, kind = move(kind_copy), text = move(text_copy), default_value = move(default_copy)]() mutable {
        auto* tab = weak_this.ptr();
        if (!tab || !tab->is_attached())
            return;
        auto* window = tab->m_root->window();
        if (!window) {
            tab->m_engine.dialog_response(tab->m_tab_id, kind.bytes_as_string_view(), false);
            return;
        }

        auto host = host_for_dialog_title(tab->current_url());
        auto make_title = [&](StringView base) -> ByteString {
            if (!host.is_empty())
                return ByteString::formatted("{} — {}", base, host);
            return base;
        };

        if (kind == "alert"sv) {
            auto title = make_title("Alert"sv);
            GUI::MessageBox::show(window, text, title.view(), GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK);
            if (auto* live = weak_this.ptr(); live && live->is_attached())
                live->m_engine.dialog_response(live->m_tab_id, "alert"sv, true);
            return;
        }

        if (kind == "confirm"sv) {
            auto title = make_title("Confirm"sv);
            auto result = GUI::MessageBox::show(window, text, title.view(), GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
            if (auto* live = weak_this.ptr(); live && live->is_attached())
                live->m_engine.dialog_response(live->m_tab_id, "confirm"sv, result == GUI::Dialog::ExecResult::Yes);
            return;
        }

        if (kind == "prompt"sv) {
            auto title = make_title("Prompt"sv);
            auto value = move(default_value);
            auto result = GUI::InputBox::show(window, value, text, title.view(), GUI::InputType::Text);
            if (auto* live = weak_this.ptr(); live && live->is_attached())
                live->m_engine.dialog_response(live->m_tab_id, "prompt"sv, result == GUI::Dialog::ExecResult::OK, value);
        }
    });
}


}
