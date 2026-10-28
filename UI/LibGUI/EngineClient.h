/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace LadybirdSDE {

// The LibGUI side of the engine connection. It owns the listening socket, starts
// the Ladybird engine host, and presents a small command/event API to the
// frontend. Frame pixels are shared through POSIX shared memory; the socket only
// carries control messages.
class EngineClient {
    AK_MAKE_NONCOPYABLE(EngineClient);

public:
    static ErrorOr<NonnullOwnPtr<EngineClient>> launch(ByteString engine_path);
    ~EngineClient();

    bool is_connected() const { return m_socket != nullptr; }

    void create_tab(Gfx::IntSize viewport_size, String const& url, u64 request_id, bool private_tab = false);
    void close_tab(u64 tab_id);
    void activate_tab(u64 tab_id);
    void navigate(u64 tab_id, String const& url, bool user_input = true);
    void go_back(u64 tab_id);
    void go_forward(u64 tab_id);
    void reload(u64 tab_id, bool bypass_cache = false);
    void stop(u64 tab_id);
    void resize(u64 tab_id, Gfx::IntSize);
    void set_focus(u64 tab_id, bool focused);
    void mouse_event(u64 tab_id, StringView type, Gfx::IntPoint position, StringView button, unsigned buttons, unsigned modifiers, int click_count = 0);
    void wheel_event(u64 tab_id, Gfx::IntPoint position, double delta_x, double delta_y, unsigned modifiers);
    void key_event(u64 tab_id, StringView type, StringView key_name, u32 code_point, unsigned modifiers, bool repeat = false);
    void dialog_response(u64 tab_id, StringView kind, bool accept, String const& text = {});
    void set_zoom(u64 tab_id, StringView direction);
    void find_in_page(u64 tab_id, String const& query);
    void find_next(u64 tab_id);
    void find_previous(u64 tab_id);
    void toggle_bookmark(u64 tab_id);
    void copy(u64 tab_id, bool cut = false);
    void paste(u64 tab_id, StringView text);
    void select_all(u64 tab_id);
    void undo(u64 tab_id);
    void redo(u64 tab_id);
    void context_menu_action(u64 tab_id, u64 action);
    void permission_response(u64 tab_id, u64 request_id, bool granted);
    void file_picker_response(u64 tab_id, String const& path);
    void select_dropdown_closed(u64 tab_id, Optional<u32> selected);
    void download_action(StringView action, u64 id);
    void quit();

    Function<void()> on_connected;
    Function<void(JsonObject const&)> on_event;
    Function<void(u64 tab_id, Gfx::Bitmap const*)> on_frame;
    Function<void()> on_disconnected;

private:
    struct FrameBuffer {
        int fd { -1 };
        void* data { nullptr };
        size_t size { 0 };
        int slots { 0 };
        size_t slot_size { 0 };
        ByteString name;
        Gfx::IntSize slot_dimensions;
        Vector<RefPtr<Gfx::Bitmap>> bitmaps;
    };

    explicit EngineClient(ByteString runtime_directory, ByteString socket_path);

    ErrorOr<void> spawn_engine(ByteString const& engine_path);
    void accept_connection(NonnullOwnPtr<Core::LocalSocket>);
    void read_available();
    void dispatch_line(StringView);
    void handle_frame_buffer(JsonObject const&);
    void handle_frame(JsonObject const&);
    void send(JsonObject const&);
    void send_simple(StringView type, u64 tab_id);
    void flush_writes();
    void close_socket();

    FrameBuffer* frame_buffer_for_tab(u64 tab_id);
    void destroy_frame_buffer(u64 tab_id);

    ByteString m_runtime_directory;
    ByteString m_socket_path;
    RefPtr<Core::LocalServer> m_server;
    OwnPtr<Core::LocalSocket> m_socket;
    Vector<u8> m_read_buffer;
    Vector<u8> m_write_buffer;
    RefPtr<Core::Notifier> m_write_notifier;
    HashMap<u64, NonnullOwnPtr<FrameBuffer>> m_frame_buffers;
    bool m_quitting { false };
};

}
