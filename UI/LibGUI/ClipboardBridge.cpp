/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardBridge.h"

#include "EngineClient.h"

#include <AK/Base64.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <string.h>

namespace LadybirdSDE {

static bool is_textual_mime(String const& name)
{
    auto view = name.bytes_as_string_view();
    return view.starts_with("text/"sv)
        || view == "application/json"sv
        || view == "application/xml"sv
        || view == "application/xhtml+xml"sv
        || view == "image/svg+xml"sv;
}

static JsonValue const* find_representation(JsonArray const& items, StringView name)
{
    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;
        auto const& object = value.as_object();
        auto item_name = object.get("name"sv);
        if (item_name.has_value() && item_name->is_string() && item_name->as_string() == name)
            return &value;
    }
    return nullptr;
}

static JsonValue const* choose_representation(JsonArray const& items)
{
    if (auto* plain = find_representation(items, "text/plain"sv))
        return plain;
    if (auto* html = find_representation(items, "text/html"sv))
        return html;

    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;
        auto const& object = value.as_object();
        auto item_name = object.get("name"sv);
        if (item_name.has_value() && item_name->is_string() && item_name->as_string().view().starts_with("image/"sv))
            return &value;
    }

    for (auto const& value : items.values()) {
        if (value.is_object() && value.as_object().has("data"sv))
            return &value;
    }

    return nullptr;
}

ClipboardBridge::ClipboardBridge(EngineClient& engine)
    : m_engine(engine)
{
}

ClipboardBridge::~ClipboardBridge()
{
    GUI::Clipboard::the().on_change = nullptr;
}

void ClipboardBridge::start()
{
    GUI::Clipboard::the().on_change = [this](ByteString const&) {
        on_clipboard_change();
    };
}

bool ClipboardBridge::matches_written(GUI::Clipboard::DataAndType const& data) const
{
    if (!m_written.has_value())
        return false;
    if (data.mime_type != m_written->mime_type)
        return false;
    if (data.data.size() != m_written->data.size())
        return false;
    if (data.data.is_empty())
        return true;
    return memcmp(data.data.data(), m_written->data.data(), data.data.size()) == 0;
}

JsonArray ClipboardBridge::items_from_data(GUI::Clipboard::DataAndType const& data)
{
    JsonArray items;
    if (data.mime_type.is_empty())
        return items;

    if (data.mime_type == "image/x-serenityos"sv) {
        auto bitmap = data.as_bitmap();
        if (!bitmap)
            return items;

        Gfx::PNGWriter::Options options;
        options.force_alpha = true;
        auto png = Gfx::PNGWriter::encode(*bitmap, options);
        if (png.is_error())
            return items;
        auto encoded = encode_base64(png.value().bytes());
        if (encoded.is_error())
            return items;

        auto encoded_string = encoded.release_value();
        JsonObject object;
        object.set("name"sv, "image/png"sv);
        object.set("data"sv, encoded_string.bytes_as_string_view());
        object.set("encoding"sv, "base64"sv);
        items.must_append(move(object));
        return items;
    }

    auto name = String::from_utf8(data.mime_type.view());
    if (name.is_error())
        return items;

    if (is_textual_mime(name.value())) {
        StringView bytes { reinterpret_cast<char const*>(data.data.data()), data.data.size() };
        if (auto text = String::from_utf8(bytes); !text.is_error()) {
            auto text_string = text.release_value();
            JsonObject object;
            object.set("name"sv, name.value().bytes_as_string_view());
            object.set("data"sv, text_string.bytes_as_string_view());
            items.must_append(move(object));
            return items;
        }
    }

    auto encoded = encode_base64(data.data.bytes());
    if (encoded.is_error())
        return items;

    auto encoded_string = encoded.release_value();
    JsonObject object;
    object.set("name"sv, name.value().bytes_as_string_view());
    object.set("data"sv, encoded_string.bytes_as_string_view());
    object.set("encoding"sv, "base64"sv);
    items.must_append(move(object));
    return items;
}

void ClipboardBridge::send_current_clipboard()
{
    auto data = GUI::Clipboard::the().fetch_data_and_type();
    if (matches_written(data))
        m_engine.set_clipboard_items(JsonArray { m_written->items });
    else
        m_engine.set_clipboard_items(items_from_data(data));
}

JsonArray ClipboardBridge::items_for_paste()
{
    auto data = GUI::Clipboard::the().fetch_data_and_type();
    if (matches_written(data))
        return JsonArray { m_written->items };

    m_written.clear();
    return items_from_data(data);
}

void ClipboardBridge::on_clipboard_change()
{
    auto data = GUI::Clipboard::the().fetch_data_and_type();

    // Writing the desktop clipboard on the engine's behalf produces a change notification of our own. The engine
    // already holds the full item, so do not replace it with the single representation we put on the desktop.
    if (matches_written(data))
        return;

    m_written.clear();
    m_engine.set_clipboard_items(items_from_data(data));
}

void ClipboardBridge::apply_engine_clipboard(JsonObject const& event)
{
    auto items = event.get_array("items"sv);
    if (!items.has_value() || items->is_empty())
        return;

    auto const* value = choose_representation(*items);
    if (!value || !value->is_object())
        return;

    auto const& object = value->as_object();
    auto name_value = object.get("name"sv);
    auto data_value = object.get("data"sv);
    if (!name_value.has_value() || !data_value.has_value())
        return;
    if (!name_value->is_string() || !data_value->is_string())
        return;

    auto mime_type = name_value->as_string();
    ByteBuffer payload;

    auto encoding = object.get("encoding"sv);
    if (encoding.has_value() && encoding->is_string() && encoding->as_string() == "base64"sv) {
        auto decoded = decode_base64(data_value->as_string());
        if (decoded.is_error())
            return;
        payload = decoded.release_value();
    } else {
        auto copied = ByteBuffer::copy(data_value->as_string().view().bytes());
        if (copied.is_error())
            return;
        payload = copied.release_value();
    }

    if (payload.is_empty())
        return;

    auto mime_view = mime_type.view();
    if (mime_view.starts_with("image/"sv) && mime_view != "image/svg+xml"sv) {
        auto decoder = Gfx::ImageDecoder::try_create_for_raw_bytes(payload.bytes(), mime_type);
        if (decoder.is_error() || !decoder.value())
            return;
        auto frame = decoder.value()->frame(0);
        if (frame.is_error() || !frame.value().image)
            return;

        auto const& bitmap = *frame.value().image;
        auto copied = ByteBuffer::copy(bitmap.scanline_u8(0), bitmap.size_in_bytes());
        if (copied.is_error())
            return;

        GUI::Clipboard::the().set_bitmap(bitmap);
        m_written = WrittenClipboard { ByteString("image/x-serenityos"sv), copied.release_value(), JsonArray { *items } };
        return;
    }

    GUI::Clipboard::the().set_data(payload.bytes(), mime_type);
    m_written = WrittenClipboard { mime_type, move(payload), JsonArray { *items } };
}

}
