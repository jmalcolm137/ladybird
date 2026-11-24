/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineServer.h"
#include "EngineApplication.h"

#include <AK/JsonArray.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWebView/CrashReportStore.h>

namespace LadybirdSDE {

static StringView compat_report_stage_name(WebView::ReportSubmission::Stage stage)
{
    switch (stage) {
    case WebView::ReportSubmission::Stage::Preparing:
        return "Preparing"sv;
    case WebView::ReportSubmission::Stage::RequestingChallenge:
        return "Requesting challenge"sv;
    case WebView::ReportSubmission::Stage::SolvingProof:
        return "Solving proof"sv;
    case WebView::ReportSubmission::Stage::Sending:
        return "Sending"sv;
    }
    VERIFY_NOT_REACHED();
}

void EngineServer::handle_compat_report_begin(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto send_failure = [this, tab_id](StringView failure, String const& reason) {
        JsonObject event;
        event.set("type"sv, "compat_report_failed"sv);
        event.set("tab"sv, tab_id);
        event.set("failure"sv, failure);
        event.set("reason"sv, reason);
        send_event(move(event));
    };

    if (!tab->view->can_report_website_issue()) {
        send_failure("validation"sv, "This page cannot be reported."_string);
        return;
    }
    if (tab->compat_report && tab->compat_report->is_sending()) {
        send_failure("validation"sv, "A report for this page is already being sent."_string);
        return;
    }

    auto page = WebView::WebCompatReport::describe_page(*tab->view);
    tab->compat_report = make<WebView::WebCompatReport>(page);
    tab->compat_report_has_page_dumps = false;
    tab->compat_report_screenshot.clear();

    if (auto bitmap = tab->view->displayed_bitmap()) {
        auto png = Gfx::PNGWriter::encode(*bitmap);
        if (!png.is_error() && png.value().size() <= WebView::WebCompatReport::maximum_screenshot_bytes)
            tab->compat_report_screenshot = png.release_value();
    }

    auto* report = tab->compat_report.ptr();
    auto dumps = WebView::WebCompatReport::request_page_dumps(*tab->view);
    dumps->when_resolved([this, tab_id](String& text) {
        auto* tab = tab_for_id(tab_id);
        if (!tab || !tab->compat_report)
            return;
        tab->compat_report->set_page_dumps(text);
        tab->compat_report_has_page_dumps = true;
        JsonObject event;
        event.set("type"sv, "compat_report_page_dumps_ready"sv);
        event.set("tab"sv, tab_id);
        send_event(move(event));
    });

    JsonObject event;
    event.set("type"sv, "compat_report_ready"sv);
    event.set("tab"sv, tab_id);
    event.set("url"sv, page.url.serialize());
    event.set("user_agent"sv, page.user_agent);
    event.set("viewport_width"sv, page.viewport_size.width());
    event.set("viewport_height"sv, page.viewport_size.height());
    event.set("device_pixel_ratio"sv, page.device_pixel_ratio);
    event.set("zoom"sv, page.zoom_level);
    event.set("has_screenshot"sv, tab->compat_report_screenshot.has_value());
    event.set("has_page_dumps"sv, tab->compat_report_has_page_dumps);

    JsonArray details;
    for (auto const& detail : report->details()) {
        JsonObject entry;
        entry.set("label"sv, detail.label);
        entry.set("value"sv, detail.value);
        entry.set("code"sv, detail.is_code);
        details.must_append(move(entry));
    }
    event.set("details"sv, move(details));
    send_event(move(event));
}

void EngineServer::handle_compat_report_diagnostics(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->compat_report)
        return;

    auto include_page_dumps = command.get_bool("include_page_dumps"sv).value_or(true)
        ? WebView::WebCompatReport::IncludePageDumps::Yes
        : WebView::WebCompatReport::IncludePageDumps::No;
    auto text = tab->compat_report->diagnostics(include_page_dumps);

    JsonObject event;
    event.set("type"sv, "compat_report_diagnostics"sv);
    event.set("tab"sv, tab_id);
    event.set("text"sv, text.view());
    send_event(move(event));
}

void EngineServer::handle_compat_report_send(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->compat_report)
        return;

    auto send_failure = [this, tab_id](StringView failure, String const& reason) {
        JsonObject event;
        event.set("type"sv, "compat_report_failed"sv);
        event.set("tab"sv, tab_id);
        event.set("failure"sv, failure);
        event.set("reason"sv, reason);
        send_event(move(event));
    };

    if (tab->compat_report->is_sending()) {
        send_failure("validation"sv, "This report is already being sent."_string);
        return;
    }

    String description;
    if (auto text = command.get_string("description"sv); text.has_value())
        description = *text;

    auto include_page_dumps = command.get_bool("include_page_dumps"sv).value_or(true);
    auto include_screenshot = command.get_bool("include_screenshot"sv).value_or(true);

    Optional<ByteBuffer> screenshot;
    if (include_screenshot && tab->compat_report_screenshot.has_value()) {
        auto copied = ByteBuffer::copy(tab->compat_report_screenshot->bytes());
        if (!copied.is_error())
            screenshot = copied.release_value();
    }

    auto* report = tab->compat_report.ptr();
    report->on_progress = [this, tab_id](WebView::ReportSubmission::Stage stage) {
        JsonObject event;
        event.set("type"sv, "compat_report_progress"sv);
        event.set("tab"sv, tab_id);
        event.set("stage"sv, compat_report_stage_name(stage));
        send_event(move(event));
    };
    report->on_retry = [this, tab_id](String reason, u32 retry, u32 maximum, u32 delay) {
        JsonObject event;
        event.set("type"sv, "compat_report_retry"sv);
        event.set("tab"sv, tab_id);
        event.set("reason"sv, reason);
        event.set("retry"sv, retry);
        event.set("maximum"sv, maximum);
        event.set("delay"sv, delay);
        send_event(move(event));
    };
    report->on_sent = [this, tab_id] {
        JsonObject event;
        event.set("type"sv, "compat_report_sent"sv);
        event.set("tab"sv, tab_id);
        send_event(move(event));
    };
    report->on_failed = [this, tab_id](WebView::ReportSubmission::Failure failure, String reason) {
        JsonObject event;
        event.set("type"sv, "compat_report_failed"sv);
        event.set("tab"sv, tab_id);
        event.set("failure"sv, failure == WebView::ReportSubmission::Failure::Preparation ? "preparation"sv : "sending"sv);
        event.set("reason"sv, reason);
        send_event(move(event));
    };

    auto error = report->send(description, move(screenshot), include_page_dumps ? WebView::WebCompatReport::IncludePageDumps::Yes : WebView::WebCompatReport::IncludePageDumps::No);
    if (error.has_value())
        send_failure("validation"sv, *error);
}

void EngineServer::handle_compat_report_retry(u64 tab_id)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab || !tab->compat_report || tab->compat_report->is_sending())
        return;
    tab->compat_report->retry();
}

void EngineServer::handle_compat_report_cancel(u64 tab_id)
{
    if (auto* tab = tab_for_id(tab_id); tab && tab->compat_report)
        tab->compat_report->cancel_sending();
}

void EngineServer::handle_compat_report_close(u64 tab_id)
{
    if (auto* tab = tab_for_id(tab_id)) {
        tab->compat_report.clear();
        tab->compat_report_screenshot.clear();
        tab->compat_report_has_page_dumps = false;
    }
}

void EngineServer::handle_certificate_info(u64 tab_id, JsonObject const&)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto url = tab->view->url();
    if (url.scheme() != "https"sv) {
        send_certificate_error(tab_id, "Certificate details are only available for secure pages."_string);
        return;
    }

    auto generation = m_next_certificate_generation++;
    m_certificate_request_generations.set(tab_id, generation);

    auto weak_this = make_weak_ptr();
    inspect_certificate_async(url, [weak_this, tab_id, generation](CertificateInspectionResult result) {
        auto* server = weak_this.ptr();
        if (!server)
            return;
        auto current = server->m_certificate_request_generations.get(tab_id);
        if (!current.has_value() || *current != generation)
            return;
        server->m_certificate_request_generations.remove(tab_id);

        if (result.details.has_value())
            server->send_certificate_info(tab_id, *result.details);
        else
            server->send_certificate_error(tab_id, result.error.is_empty() ? "The server did not present a certificate."_string : result.error);
    });
}

void EngineServer::send_certificate_info(u64 tab_id, CertificateDetails const& info)
{
    JsonObject event;
    event.set("type"sv, "certificate_info"sv);
    event.set("tab"sv, tab_id);
    event.set("ok"sv, true);
    event.set("subject"sv, info.subject);
    event.set("issuer"sv, info.issuer);
    event.set("valid_from"sv, info.valid_from);
    event.set("valid_until"sv, info.valid_until);
    event.set("serial_number"sv, info.serial_number);
    event.set("sha256_fingerprint"sv, info.sha256_fingerprint);
    event.set("protocol"sv, info.protocol);
    event.set("cipher"sv, info.cipher);
    send_event(move(event));
}

void EngineServer::send_certificate_error(u64 tab_id, String const& error)
{
    JsonObject event;
    event.set("type"sv, "certificate_info"sv);
    event.set("tab"sv, tab_id);
    event.set("ok"sv, false);
    event.set("error"sv, error);
    send_event(move(event));
}


void EngineServer::send_crash_report_available(ByteString const& report_name)
{
    if (report_name.is_empty())
        return;
    auto name = String::from_byte_string(report_name).release_value();
    if (m_offered_crash_reports.contains(name))
        return;
    m_offered_crash_reports.set(move(name));

    JsonObject event;
    event.set("type"sv, "crash_report_available"sv);
    event.set("name"sv, report_name.view());
    send_event(move(event));
}

void EngineServer::check_crash_reports()
{
    auto names = WebView::CrashReportStore::the().pending_report_names();
    if (names.is_error())
        return;
    for (auto const& name : names.value())
        send_crash_report_available(name);
}

static void append_crash_report_fields(JsonArray& fields, Vector<WebView::ReportDetail> const& details)
{
    for (auto const& detail : details) {
        JsonObject field;
        field.set("label"sv, detail.label);
        field.set("value"sv, detail.value);
        field.set("code"sv, detail.is_code);
        fields.must_append(move(field));
    }
}

void EngineServer::send_crash_report_ready(WebView::CrashReportReview::Report const& report)
{
    JsonArray fields;
    append_crash_report_fields(fields, report.fields);

    JsonObject event;
    event.set("type"sv, "crash_report_ready"sv);
    event.set("name"sv, report.name.view());
    event.set("fields"sv, move(fields));

    auto names = WebView::CrashReportStore::the().pending_report_names();
    bool has_next = false;
    if (!names.is_error()) {
        for (auto const& name : names.value()) {
            if (name != report.name) {
                has_next = true;
                break;
            }
        }
    }
    event.set("has_next"sv, has_next);
    send_event(move(event));
}

void EngineServer::send_crash_report_failure(StringView failure, String const& reason)
{
    JsonObject event;
    event.set("type"sv, "crash_report_failed"sv);
    event.set("failure"sv, failure);
    event.set("reason"sv, reason);
    send_event(move(event));
}

void EngineServer::handle_crash_report_open(JsonObject const& command)
{
    if (m_crash_report && m_crash_report->is_sending())
        return;

    if (!m_crash_report)
        m_crash_report = make<WebView::CrashReportReview>();

    m_crash_report->on_progress = [this](WebView::ReportSubmission::Stage stage) {
        JsonObject event;
        event.set("type"sv, "crash_report_progress"sv);
        event.set("stage"sv, compat_report_stage_name(stage));
        send_event(move(event));
    };
    m_crash_report->on_retry = [this](String reason, u32 retry, u32 maximum, u32 delay) {
        JsonObject event;
        event.set("type"sv, "crash_report_retry"sv);
        event.set("reason"sv, reason);
        event.set("retry"sv, retry);
        event.set("maximum"sv, maximum);
        event.set("delay"sv, delay);
        send_event(move(event));
    };
    m_crash_report->on_sent = [this] {
        JsonObject event;
        event.set("type"sv, "crash_report_sent"sv);
        send_event(move(event));
    };
    m_crash_report->on_failed = [this](WebView::ReportSubmission::Failure failure, String reason) {
        send_crash_report_failure(failure == WebView::ReportSubmission::Failure::Preparation ? "preparation"sv : "sending"sv, reason);
    };

    Optional<ByteString> name;
    if (auto requested = command.get_string("name"sv); requested.has_value() && !requested->is_empty())
        name = requested->to_byte_string();

    if (!name.has_value()) {
        auto pending = WebView::CrashReportStore::the().pending_report_names();
        if (pending.is_error() || pending.value().is_empty()) {
            send_crash_report_failure("open"sv, "There are no saved crash reports."_string);
            return;
        }
        name = pending.value().first();
    }

    auto report = m_crash_report->open(*name);
    if (report.is_error()) {
        send_crash_report_failure("open"sv, String::from_utf8(report.error().string_literal()).release_value());
        return;
    }
    send_crash_report_ready(report.value());
}

void EngineServer::handle_crash_report_next()
{
    if (!m_crash_report) {
        handle_crash_report_open({});
        return;
    }

    auto current = m_crash_report->report_name();
    auto pending = WebView::CrashReportStore::the().pending_report_names();
    if (!pending.is_error()) {
        for (auto const& name : pending.value()) {
            if (name == current)
                continue;
            JsonObject command;
            command.set("name"sv, name.view());
            handle_crash_report_open(command);
            return;
        }
    }

    JsonObject event;
    event.set("type"sv, "crash_report_dismissed"sv);
    send_event(move(event));
}

void EngineServer::handle_crash_report_text()
{
    if (!m_crash_report)
        return;
    auto text = m_crash_report->report_text();
    if (text.is_error()) {
        send_crash_report_failure("preparation"sv, String::from_utf8(text.error().string_literal()).release_value());
        return;
    }
    JsonObject event;
    event.set("type"sv, "crash_report_text"sv);
    event.set("text"sv, text.value().view());
    send_event(move(event));
}

void EngineServer::handle_crash_report_send(JsonObject const& command)
{
    if (!m_crash_report || m_crash_report->is_sending())
        return;

    auto description = command.get_string("description"sv).value_or({});
    Optional<String> url;
    if (command.get_bool("include_website"sv).value_or(false))
        url = command.get_string("url"sv).value_or(String {});

    auto error = m_crash_report->send(description, url);
    if (error.has_value())
        send_crash_report_failure("validation"sv, *error);
}

void EngineServer::handle_crash_report_cancel()
{
    if (m_crash_report)
        m_crash_report->cancel_sending();
    JsonObject event;
    event.set("type"sv, "crash_report_cancelled"sv);
    send_event(move(event));
}

void EngineServer::handle_crash_report_close()
{
    m_crash_report.clear();
}

void EngineServer::handle_crash_report_dismiss()
{
    if (auto names = WebView::CrashReportStore::the().pending_report_names(); !names.is_error()) {
        for (auto const& name : names.value()) {
            if (auto result = WebView::CrashReportStore::the().mark_seen(name); result.is_error())
                warnln("Could not mark crash report as seen: {}", result.error());
        }
    }
    m_offered_crash_reports.clear();
    m_crash_report.clear();
    JsonObject event;
    event.set("type"sv, "crash_report_dismissed"sv);
    send_event(move(event));
}


void EngineServer::schedule_crash_report_check()
{
    if (m_quitting)
        return;
    if (!m_crash_report_check_timer) {
        m_crash_report_check_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1500), [this] {
            check_crash_reports();
        });
    }
    m_crash_report_check_timer->restart(AK::Duration::from_milliseconds(1500));
}


}
