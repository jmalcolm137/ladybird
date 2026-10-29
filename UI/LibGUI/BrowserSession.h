/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "EngineClient.h"

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace LadybirdSDE {

class BrowserWindow;

// Owns the one engine host for this frontend process and every LibGUI window
// attached to it. Windows share tabs, bookmarks, settings, history, and the
// engine profile; a window is only a view over a subset of the host's tabs.
class BrowserSession {
    AK_MAKE_NONCOPYABLE(BrowserSession);

public:
    static ErrorOr<NonnullOwnPtr<BrowserSession>> create(Vector<String> initial_urls = {}, bool private_window = false);
    ~BrowserSession();

    void show();
    void quit();

    BrowserWindow* create_window(Vector<String> urls = {}, bool private_window = false);
    void restore_previous_session();

    // Tabs closed by the user, newest last. Private tabs are not retained.
    void note_closed_tab(String const& url, String const& title, bool private_tab);
    bool reopen_closed_tab();
    size_t closed_tab_count() const { return m_closed_tabs.size(); }

    u64 next_request_id() { return m_next_request_id++; }
    EngineClient& engine() { return *m_engine; }

    // Called by a BrowserWindow when its native window is closing.
    void window_will_close(BrowserWindow&);

private:
    BrowserSession() = default;

    ErrorOr<void> initialize(Vector<String> initial_urls, bool private_window);
    BrowserWindow* create_window_internal(Vector<String> urls, bool private_window, Optional<Gfx::IntSize> size = {}, Optional<Gfx::IntPoint> position = {}, Optional<size_t> active_tab = {}, bool maximized = false, bool lazy_load = false);

    BrowserWindow* window_for_tab(u64 tab_id);
    BrowserWindow* window_for_id(u64 window_id);
    BrowserWindow* window_for_request(u64 request_id);
    BrowserWindow* active_window() const;

    void on_engine_connected();
    void on_engine_event(JsonObject const&);
    void handle_session_plan(JsonObject const&);
    void on_engine_frame(u64 tab_id, Gfx::Bitmap const*, u64 sequence, Gfx::IntRect damage, bool full);
    void on_engine_disconnected();
    void handle_open_window(JsonObject const&);

    ByteString session_path() const;
    bool should_restore_session() const;
    void save_session();
    bool load_session();
    bool load_legacy_session(JsonObject const&);

    struct ClosedTab {
        String url;
        String title;
    };

    OwnPtr<EngineClient> m_engine;
    Vector<NonnullOwnPtr<BrowserWindow>> m_windows;
    Vector<ClosedTab> m_closed_tabs;
    u64 m_next_request_id { 1 };
    u64 m_next_window_id { 1 };
    RefPtr<Core::Timer> m_autosave_timer;
    RefPtr<Core::Timer> m_session_plan_timeout;
    bool m_waiting_for_session_plan { false };
    bool m_session_plan_received { false };
    Vector<String> m_pending_initial_urls;
};

}
