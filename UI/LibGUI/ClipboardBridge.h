/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibGUI/Clipboard.h>

namespace LadybirdSDE {

class EngineClient;

// Moves clipboard content between the desktop clipboard and the engine host.
//
// The desktop clipboard owns the bytes and speaks one MIME type per item; the engine speaks Web's list of
// representations. When the engine writes a rich item (text/plain + text/html + image/png), we put the best single
// representation on the desktop clipboard but remember the whole item, so pasting back into the browser preserves
// formatting. An external clipboard change invalidates that memory.
class ClipboardBridge {
    AK_MAKE_NONCOPYABLE(ClipboardBridge);
    AK_MAKE_NONMOVABLE(ClipboardBridge);

public:
    explicit ClipboardBridge(EngineClient&);
    ~ClipboardBridge();

    void start();
    void send_current_clipboard();
    JsonArray items_for_paste();
    void apply_engine_clipboard(JsonObject const& event);

private:
    struct WrittenClipboard {
        ByteString mime_type;
        ByteBuffer data;
        JsonArray items;
    };

    void on_clipboard_change();
    bool matches_written(GUI::Clipboard::DataAndType const&) const;
    static JsonArray items_from_data(GUI::Clipboard::DataAndType const&);

    EngineClient& m_engine;
    Optional<WrittenClipboard> m_written;
};

}
