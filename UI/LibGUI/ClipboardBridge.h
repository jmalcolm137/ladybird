/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>

namespace LadybirdSDE {

class EngineClient;

// Moves clipboard content between the desktop clipboard and the engine host.
//
// The desktop clipboard stores a list of representations (text/plain and text/html together, images, file lists),
// matching the engine's Web clipboard item. Images cross the boundary as image/png; the LibWM clipboard portal
// converts them to and from the SDE raw image format for local applications.
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
    JsonArray items_from_clipboard() const;
    void on_clipboard_change();

    EngineClient& m_engine;
};

}
