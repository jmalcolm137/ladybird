/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineServer.h"

#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

namespace LadybirdSDE {

static Web::UIEvents::KeyModifier modifiers_from_command(JsonObject const& command)
{
    auto modifiers = Web::UIEvents::KeyModifier::Mod_None;
    if (command.get_bool("ctrl"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (command.get_bool("shift"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    if (command.get_bool("alt"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (command.get_bool("super"sv).value_or(false))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Super;
    return modifiers;
}

void EngineServer::handle_mouse(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("event"sv);
    if (!kind.has_value())
        return;

    auto scale = max(tab->device_pixel_ratio, 1);
    Web::MouseEvent event;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)) * scale, static_cast<int>(command.get_i64("y"sv).value_or(0)) * scale };
    event.screen_position = event.position;
    event.modifiers = modifiers_from_command(command);

    auto button = command.get_string("button"sv);
    auto button_name = button.has_value() ? button->bytes_as_string_view() : "none"sv;
    if (button_name == "left"sv)
        event.button = Web::UIEvents::MouseButton::Primary;
    else if (button_name == "right"sv)
        event.button = Web::UIEvents::MouseButton::Secondary;
    else if (button_name == "middle"sv)
        event.button = Web::UIEvents::MouseButton::Middle;
    else if (button_name == "back"sv)
        event.button = Web::UIEvents::MouseButton::Backward;
    else if (button_name == "forward"sv)
        event.button = Web::UIEvents::MouseButton::Forward;
    else
        event.button = Web::UIEvents::MouseButton::None;

    event.buttons = static_cast<Web::UIEvents::MouseButton>(command.get_u64("buttons"sv).value_or(0));
    event.click_count = static_cast<int>(command.get_i64("click_count"sv).value_or(0));

    if (*kind == "down"sv)
        event.type = Web::MouseEvent::Type::MouseDown;
    else if (*kind == "up"sv)
        event.type = Web::MouseEvent::Type::MouseUp;
    else if (*kind == "move"sv)
        event.type = Web::MouseEvent::Type::MouseMove;
    else if (*kind == "leave"sv)
        event.type = Web::MouseEvent::Type::MouseLeave;
    else
        return;

    tab->view->enqueue_input_event(move(event));
}

void EngineServer::handle_wheel(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto scale = max(tab->device_pixel_ratio, 1);
    Web::MouseEvent event;
    event.type = Web::MouseEvent::Type::MouseWheel;
    event.position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)) * scale, static_cast<int>(command.get_i64("y"sv).value_or(0)) * scale };
    event.screen_position = event.position;
    event.modifiers = modifiers_from_command(command);
    event.button = Web::UIEvents::MouseButton::None;
    event.buttons = Web::UIEvents::MouseButton::None;

    event.wheel_delta_x = command.get_double_with_precision_loss("delta_x"sv).value_or(0.0);
    event.wheel_delta_y = command.get_double_with_precision_loss("delta_y"sv).value_or(0.0);
    event.wheel_delta_precision = Web::WheelDeltaPrecision::Discrete;
    event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, event.wheel_delta_precision);
    event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, event.wheel_delta_precision);

    if (trace_frames())
        warnln("LadybirdEngine: tab {} wheel delta {}x{}", tab_id, event.wheel_delta_x, event.wheel_delta_y);
    ++tab->wheel_events_since_frame;
    tab->view->enqueue_input_event(move(event));
    note_user_input(*tab);
}

void EngineServer::handle_key(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto kind = command.get_string("event"sv);
    if (!kind.has_value())
        return;

    Web::KeyEvent event;
    if (*kind == "down"sv)
        event.type = Web::KeyEvent::Type::KeyDown;
    else if (*kind == "up"sv)
        event.type = Web::KeyEvent::Type::KeyUp;
    else
        return;

    auto name = command.get_string("key"sv);
    auto key_string = Utf16String::from_utf8_with_replacement_character(name.has_value() ? name->bytes_as_string_view() : ""sv);
    event.key = Web::UIEvents::key_code_from_string(key_string.utf16_view());
    event.modifiers = modifiers_from_command(command);
    event.code_point = static_cast<u32>(command.get_u64("codepoint"sv).value_or(0));
    event.repeat = command.get_bool("repeat"sv).value_or(false);

    // A printable key press with no command modifier is text for the page. The
    // platform's input method may later drive composition, but this covers
    // ordinary typing in the LibGUI frontend.
    auto has_command_modifier = (event.modifiers & (Web::UIEvents::KeyModifier::Mod_Ctrl | Web::UIEvents::KeyModifier::Mod_Alt | Web::UIEvents::KeyModifier::Mod_Super)) != Web::UIEvents::KeyModifier::Mod_None;
    event.should_insert_text = event.type == Web::KeyEvent::Type::KeyDown && event.code_point != 0 && !has_command_modifier;
    auto should_watch_for_frame = event.type == Web::KeyEvent::Type::KeyDown && !event.repeat;

    tab->view->enqueue_input_event(move(event));
    if (should_watch_for_frame)
        note_user_input(*tab);
}


void EngineServer::handle_ime_marked_text(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;
    String text;
    if (auto value = command.get_string("text"sv); value.has_value())
        text = *value;
    tab->view->set_marked_text_from_input_method(Utf16String::from_utf8_with_replacement_character(text));
}

void EngineServer::handle_ime_commit_text(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;
    String text;
    if (auto value = command.get_string("text"sv); value.has_value())
        text = *value;
    auto replacement_start = static_cast<i32>(command.get_i64("replacement_start"sv).value_or(0));
    auto replacement_length = static_cast<i32>(command.get_i64("replacement_length"sv).value_or(0));
    tab->view->commit_text_from_input_method(Utf16String::from_utf8_with_replacement_character(text), replacement_start, replacement_length);
}

void EngineServer::handle_ime_unmark_text(u64 tab_id)
{
    if (auto* tab = tab_for_id(tab_id))
        tab->view->unmark_text_from_input_method();
}

void EngineServer::handle_drag(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto type_name = command.get_string("drag_type"sv);
    if (!type_name.has_value())
        return;

    Web::DragEvent::Type type;
    if (*type_name == "start"sv)
        type = Web::DragEvent::Type::DragStart;
    else if (*type_name == "move"sv)
        type = Web::DragEvent::Type::DragMove;
    else if (*type_name == "end"sv)
        type = Web::DragEvent::Type::DragEnd;
    else if (*type_name == "drop"sv)
        type = Web::DragEvent::Type::Drop;
    else
        return;

    auto scale = max(tab->device_pixel_ratio, 1);
    Web::DragEvent event {
        .type = type,
        .position = Web::DevicePixelPoint { static_cast<int>(command.get_i64("x"sv).value_or(0)) * scale, static_cast<int>(command.get_i64("y"sv).value_or(0)) * scale },
        .screen_position = {},
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .modifiers = modifiers_from_command(command),
        .files = {},
        .text = {},
        .urls = {},
        .browser_data = {},
    };
    event.screen_position = event.position;

    if (auto text = command.get_string("text"sv); text.has_value() && !text->is_empty())
        event.text = *text;

    if (auto urls = command.get_array("urls"sv); urls.has_value()) {
        for (auto const& value : urls->values()) {
            if (!value.is_string())
                continue;
            String url_string = value.as_string();
            auto parsed = URL::Parser::basic_parse(url_string.bytes_as_string_view());
            if (!parsed.has_value())
                continue;
            if (parsed->scheme() == "file"sv) {
                static size_t const max_drop_file_bytes = [] {
                    if (auto configured = Core::Environment::get("LADYBIRD_SDE_MAX_DROP_FILE_BYTES"sv); configured.has_value()) {
                        if (auto parsed_limit = configured->to_number<size_t>(); parsed_limit.has_value())
                            return *parsed_limit;
                    }
                    return static_cast<size_t>(64 * MiB);
                }();
                auto path = parsed->file_path();
                if (path.is_empty())
                    continue;
                auto metadata = Core::System::stat(path);
                if (metadata.is_error() || !S_ISREG(metadata.value().st_mode) || static_cast<u64>(metadata.value().st_size) > max_drop_file_bytes) {
                    warnln("LadybirdEngine: skipped dropped file '{}' (not a regular file or exceeds {} bytes)", path, max_drop_file_bytes);
                    continue;
                }
                auto file = Core::File::open(path, Core::File::OpenMode::Read);
                if (file.is_error())
                    continue;
                auto contents = file.value()->read_until_eof();
                if (contents.is_error())
                    continue;
                auto name = Utf16String::from_utf8(LexicalPath::basename(path));
                event.files.append(Web::HTML::SelectedFile { move(name), contents.release_value() });
            } else {
                event.urls.append(move(url_string));
            }
        }
    }

    tab->view->enqueue_input_event(move(event));
}


}
