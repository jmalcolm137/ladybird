/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ClipboardBridge.h"

#include "EngineClient.h"

#include <AK/Base64.h>
#include <AK/String.h>
#include <LibGUI/Clipboard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>

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

static void append_representation(JsonArray& items, ByteString const& mime_type, ReadonlyBytes data)
{
    if (data.is_empty())
        return;

    auto name = String::from_utf8(mime_type.view());
    if (name.is_error())
        name = "application/octet-stream"_string;

    if (is_textual_mime(name.value())) {
        StringView bytes { reinterpret_cast<char const*>(data.data()), data.size() };
        if (auto text = String::from_utf8(bytes); !text.is_error()) {
            auto text_string = text.release_value();
            JsonObject object;
            object.set("name"sv, name.value().bytes_as_string_view());
            object.set("data"sv, text_string.bytes_as_string_view());
            items.must_append(move(object));
            return;
        }
    }

    auto encoded = encode_base64(data);
    if (encoded.is_error())
        return;

    auto encoded_string = encoded.release_value();
    JsonObject object;
    object.set("name"sv, name.value().bytes_as_string_view());
    object.set("data"sv, encoded_string.bytes_as_string_view());
    object.set("encoding"sv, "base64"sv);
    items.must_append(move(object));
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

JsonArray ClipboardBridge::items_from_clipboard() const
{
    JsonArray items;

    for (auto const& entry : GUI::Clipboard::the().fetch_entries()) {
        if (entry.data.is_empty())
            continue;

        // Local applications see images in the SDE raw format; WebContent wants PNG.
        if (entry.mime_type == "image/x-serenityos"sv) {
            auto bitmap = entry.as_bitmap();
            if (!bitmap)
                continue;

            Gfx::PNGWriter::Options options;
            options.force_alpha = true;
            auto png = Gfx::PNGWriter::encode(*bitmap, options);
            if (png.is_error())
                continue;

            append_representation(items, "image/png"sv, png.value().bytes());
            continue;
        }

        append_representation(items, entry.mime_type, entry.data.bytes());
    }

    return items;
}

void ClipboardBridge::send_current_clipboard()
{
    m_engine.set_clipboard_items(items_from_clipboard());
}

JsonArray ClipboardBridge::items_for_paste()
{
    return items_from_clipboard();
}

void ClipboardBridge::on_clipboard_change()
{
    m_engine.set_clipboard_items(items_from_clipboard());
}

void ClipboardBridge::apply_engine_clipboard(JsonObject const& event)
{
    auto items = event.get_array("items"sv);
    if (!items.has_value())
        return;

    Vector<GUI::ClipboardEntry> entries;

    for (auto const& value : items->values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        auto name_value = object.get("name"sv);
        auto data_value = object.get("data"sv);
        if (!name_value.has_value() || !data_value.has_value())
            continue;
        if (!name_value->is_string() || !data_value->is_string())
            continue;

        ByteBuffer payload;
        auto encoding = object.get("encoding"sv);
        if (encoding.has_value() && encoding->is_string() && encoding->as_string() == "base64"sv) {
            auto decoded = decode_base64(data_value->as_string());
            if (decoded.is_error())
                continue;
            payload = decoded.release_value();
        } else {
            auto copied = ByteBuffer::copy(data_value->as_string().view().bytes());
            if (copied.is_error())
                continue;
            payload = copied.release_value();
        }

        if (payload.is_empty())
            continue;

        entries.append({ move(payload), name_value->as_string(), {} });
    }

    GUI::Clipboard::the().set_entries(move(entries));
}

}
