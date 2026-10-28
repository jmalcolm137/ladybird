/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineClient.h"

#include <AK/Array.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGUI/Event.h>
#include <Shared/EngineProtocol.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace LadybirdSDE {

EngineClient::EngineClient(ByteString runtime_directory, ByteString socket_path)
    : m_runtime_directory(move(runtime_directory))
    , m_socket_path(move(socket_path))
{
}

EngineClient::~EngineClient()
{
    if (m_socket) {
        JsonObject quit;
        quit.set("type"sv, "quit"sv);
        send(quit);
        flush_writes();
    }
    close_socket();

    while (!m_frame_buffers.is_empty())
        destroy_frame_buffer(m_frame_buffers.begin()->key);

    m_server = nullptr;
    (void)Core::System::unlink(m_socket_path);
    (void)Core::System::rmdir(m_runtime_directory);
}

ErrorOr<NonnullOwnPtr<EngineClient>> EngineClient::launch(ByteString engine_path)
{
    auto runtime_root = TRY(Core::StandardPaths::runtime_directory());
    auto runtime_directory = ByteString::formatted("{}/ladybird-sde-{}", runtime_root, ::getpid());
    TRY(Core::Directory::create(runtime_directory, Core::Directory::CreateDirectories::Yes, 0700));

    auto socket_path = ByteString::formatted("{}/engine.sock", runtime_directory);
    (void)Core::System::unlink(socket_path);

    auto client = adopt_own(*new EngineClient(runtime_directory, socket_path));

    auto server = Core::LocalServer::construct();
    server->on_accept = [client = client.ptr()](NonnullOwnPtr<Core::LocalSocket> socket) {
        client->accept_connection(move(socket));
    };
    if (!server->listen(socket_path))
        return Error::from_string_literal("Could not listen on the Ladybird engine socket");

    client->m_server = move(server);
    TRY(client->spawn_engine(engine_path));
    return client;
}

ErrorOr<void> EngineClient::spawn_engine(ByteString const& engine_path)
{
    Vector<ByteString> arguments;
    arguments.append("--engine-socket"sv);
    arguments.append(m_socket_path);
    arguments.append("--frame-directory"sv);
    arguments.append(m_runtime_directory);
    // Keep the LibGUI browser's profile separate from a Qt Ladybird running at
    // the same time. The engine host is a full Ladybird UI process and would
    // otherwise share its settings database and profile lock. Set
    // LADYBIRD_SDE_PROFILE=default to share the default Ladybird profile, or to
    // a name to pick another profile.
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    auto profile_name = profile.has_value() ? profile->to_byte_string() : ByteString { "libgui" };
    if (profile_name != "default"sv) {
        arguments.append("--profile"sv);
        arguments.append(profile_name);
    }
    if (Core::Environment::has("LADYBIRD_SDE_DISABLE_SANDBOX"sv))
        arguments.append("--disable-sandbox"sv);

    // New windows opened from an engine context menu start another LibGUI
    // frontend, so tell the engine where this executable lives.
    if (auto executable = Core::System::current_executable_path(); executable.is_error())
        warnln("Ladybird: could not resolve the frontend path: {}", executable.error());
    else
        (void)Core::Environment::set("LADYBIRD_SDE_FRONTEND"sv, executable.value(), Core::Environment::Overwrite::Yes);

    // The engine host inherits the environment. It finds WebContent and the
    // resource tree relative to its own path, so the wheel does not need a
    // special prefix here.
    TRY(Core::Process::spawn(engine_path, arguments, {}, Core::Process::KeepAsChild::Yes));
    return {};
}

void EngineClient::accept_connection(NonnullOwnPtr<Core::LocalSocket> socket)
{
    if (m_socket) {
        // One window, one engine host.
        socket->close();
        return;
    }

    // Events are read in a loop until the socket would block, so it must not block.
    if (auto result = socket->set_blocking(false); result.is_error()) {
        warnln("Ladybird: could not configure the engine socket: {}", result.error());
        socket->close();
        return;
    }

    m_socket = move(socket);
    m_socket->on_ready_to_read = [this] {
        read_available();
    };

    if (auto fd = m_socket->fd(); fd.has_value()) {
        m_write_notifier = Core::Notifier::construct(*fd, Core::Notifier::Type::Write);
        m_write_notifier->on_activation = [this] {
            flush_writes();
        };
        m_write_notifier->set_enabled(false);
    }

    if (on_connected)
        on_connected();
}

void EngineClient::read_available()
{
    if (!m_socket)
        return;

    Array<u8, 64 * 1024> buffer;
    while (true) {
        auto result = m_socket->read_some(buffer.span());
        if (result.is_error()) {
            if (result.error().is_errno() && (result.error().code() == EAGAIN || result.error().code() == EWOULDBLOCK))
                return;
            close_socket();
            if (on_disconnected)
                on_disconnected();
            return;
        }

        auto bytes = result.release_value();
        if (bytes.is_empty()) {
            close_socket();
            if (on_disconnected)
                on_disconnected();
            return;
        }

        m_read_buffer.append(bytes.data(), bytes.size());

        while (true) {
            auto newline_index = m_read_buffer.find_first_index('\n');
            if (!newline_index.has_value())
                break;

            auto line_length = *newline_index;
            auto line_view = StringView { reinterpret_cast<char const*>(m_read_buffer.data()), line_length };
            if (!line_view.is_empty() && line_view.ends_with('\r'))
                line_view = line_view.substring_view(0, line_view.length() - 1);

            // Remove the command from the buffer before dispatching it. A
            // dispatch can open a nested event loop (a file picker or a menu),
            // during which the socket notifier can call read_available again.
            auto line = String::from_utf8(line_view);
            m_read_buffer.remove(0, line_length + 1);
            if (line.is_error()) {
                warnln("Ladybird: could not decode engine event: {}", line.error());
                continue;
            }
            dispatch_line(line.value().bytes_as_string_view());
        }
    }
}

void EngineClient::dispatch_line(StringView line)
{
    auto parsed = JsonParser { line }.parse();
    if (parsed.is_error()) {
        warnln("Ladybird: could not parse engine event: {}", parsed.error());
        return;
    }

    auto value = parsed.release_value();
    if (!value.is_object())
        return;

    auto const& event = value.as_object();
    auto type = event.get_byte_string("type"sv);
    if (!type.has_value())
        return;

    if (*type == "hello"sv) {
        auto version = event.get_u64("protocol"sv);
        if (!version.has_value() || *version != protocol_version) {
            warnln("Ladybird: engine protocol mismatch (frontend {}, engine {})", protocol_version, version.value_or(0));
            if (on_disconnected)
                on_disconnected();
        }
        return;
    }

    if (*type == "frame_buffer"sv) {
        handle_frame_buffer(event);
        return;
    }
    if (*type == "frame"sv) {
        handle_frame(event);
        return;
    }

    if (on_event)
        on_event(event);
}

EngineClient::FrameBuffer* EngineClient::frame_buffer_for_tab(u64 tab_id)
{
    auto it = m_frame_buffers.find(tab_id);
    if (it == m_frame_buffers.end())
        return nullptr;
    return it->value.ptr();
}

void EngineClient::destroy_frame_buffer(u64 tab_id)
{
    auto buffer = m_frame_buffers.take(tab_id);
    if (!buffer.has_value())
        return;

    if (on_frame)
        on_frame(tab_id, nullptr);

    (*buffer)->bitmaps.clear();
    if ((*buffer)->data)
        (void)Core::System::munmap((*buffer)->data, (*buffer)->size);
    if ((*buffer)->fd >= 0)
        ::close((*buffer)->fd);
    if (!(*buffer)->name.is_empty())
        ::shm_unlink((*buffer)->name.characters());
}

void EngineClient::handle_frame_buffer(JsonObject const& event)
{
    auto tab_id = event.get_u64("tab"sv);
    auto name = event.get_byte_string("name"sv);
    auto slots = event.get_i64("slots"sv);
    auto slot_size = event.get_u64("slot_size"sv);
    auto width = event.get_i64("width"sv);
    auto height = event.get_i64("height"sv);

    if (!tab_id.has_value() || !name.has_value() || !slots.has_value() || !slot_size.has_value() || !width.has_value() || !height.has_value())
        return;

    destroy_frame_buffer(*tab_id);

    int fd = ::shm_open(name->characters(), O_RDWR, 0600);
    if (fd < 0) {
        warnln("Ladybird: could not open frame buffer '{}': {}", *name, strerror(errno));
        return;
    }

    auto size = static_cast<size_t>(*slot_size) * static_cast<size_t>(*slots);
    auto mapping = Core::System::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping.is_error()) {
        warnln("Ladybird: could not map frame buffer: {}", mapping.error());
        ::close(fd);
        return;
    }

    auto buffer = make<FrameBuffer>();
    buffer->fd = fd;
    buffer->data = mapping.value();
    buffer->size = size;
    buffer->slots = static_cast<int>(*slots);
    buffer->slot_size = *slot_size;
    buffer->name = *name;
    buffer->slot_dimensions = { static_cast<int>(*width), static_cast<int>(*height) };
    buffer->bitmaps.ensure_capacity(buffer->slots);
    for (int i = 0; i < buffer->slots; ++i)
        buffer->bitmaps.append(RefPtr<Gfx::Bitmap> {});

    m_frame_buffers.set(*tab_id, move(buffer));
}

void EngineClient::handle_frame(JsonObject const& event)
{
    auto tab_id = event.get_u64("tab"sv);
    auto slot = event.get_i64("slot"sv);
    auto width = event.get_i64("width"sv);
    auto height = event.get_i64("height"sv);
    auto stride = event.get_u64("stride"sv);

    if (!tab_id.has_value() || !slot.has_value() || !width.has_value() || !height.has_value() || !stride.has_value())
        return;

    auto* buffer = frame_buffer_for_tab(*tab_id);
    if (!buffer || *slot < 0 || *slot >= buffer->slots)
        return;

    if (static_cast<size_t>(*width) * 4 > *stride || static_cast<size_t>(*height) * *stride > buffer->slot_size)
        return;

    auto size = Gfx::IntSize { static_cast<int>(*width), static_cast<int>(*height) };
    auto& bitmap = buffer->bitmaps[*slot];

    if (!bitmap || bitmap->size() != size || bitmap->pitch() != *stride) {
        auto* pixels = static_cast<u8*>(buffer->data) + (static_cast<size_t>(*slot) * buffer->slot_size);
        auto created = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, size, 1, *stride, pixels);
        if (created.is_error()) {
            warnln("Ladybird: could not wrap frame: {}", created.error());
            return;
        }
        bitmap = created.release_value();
    }

    if (on_frame)
        on_frame(*tab_id, bitmap.ptr());
}

void EngineClient::send(JsonObject const& event)
{
    if (!m_socket)
        return;

    auto text = event.to_byte_string();
    StringBuilder builder;
    builder.append(text);
    builder.append('\n');
    auto bytes = builder.string_view().bytes();
    m_write_buffer.append(bytes.data(), bytes.size());
    flush_writes();
}

void EngineClient::flush_writes()
{
    if (!m_socket) {
        m_write_buffer.clear();
        return;
    }

    while (!m_write_buffer.is_empty()) {
        auto result = m_socket->write_some(m_write_buffer.span());
        if (result.is_error()) {
            if (result.error().is_errno() && (result.error().code() == EAGAIN || result.error().code() == EWOULDBLOCK)) {
                if (m_write_notifier)
                    m_write_notifier->set_enabled(true);
                return;
            }

            warnln("Ladybird: could not write to engine: {}", result.error());
            close_socket();
            if (on_disconnected)
                on_disconnected();
            return;
        }

        auto written = result.value();
        if (written == 0) {
            close_socket();
            if (on_disconnected)
                on_disconnected();
            return;
        }
        m_write_buffer.remove(0, written);
    }

    if (m_write_notifier)
        m_write_notifier->set_enabled(false);
}

void EngineClient::close_socket()
{
    if (m_write_notifier) {
        m_write_notifier->close();
        m_write_notifier.clear();
    }
    if (m_socket) {
        m_socket->close();
        m_socket.clear();
    }
    m_write_buffer.clear();
}

void EngineClient::send_simple(StringView type, u64 tab_id)
{
    JsonObject event;
    event.set("type"sv, type);
    event.set("tab"sv, tab_id);
    send(event);
}

void EngineClient::create_tab(Gfx::IntSize viewport_size, String const& url, u64 request_id, bool private_tab)
{
    JsonObject event;
    event.set("type"sv, "create_tab"sv);
    event.set("request"sv, request_id);
    event.set("width"sv, viewport_size.width());
    event.set("height"sv, viewport_size.height());
    event.set("url"sv, url.bytes_as_string_view());
    event.set("private"sv, private_tab);
    send(event);
}

void EngineClient::close_tab(u64 tab_id) { send_simple("close_tab"sv, tab_id); }
void EngineClient::activate_tab(u64 tab_id) { send_simple("activate"sv, tab_id); }

void EngineClient::navigate(u64 tab_id, String const& url, bool user_input)
{
    JsonObject event;
    event.set("type"sv, "navigate"sv);
    event.set("tab"sv, tab_id);
    event.set("url"sv, url.bytes_as_string_view());
    event.set("user_input"sv, user_input);
    send(event);
}

void EngineClient::go_back(u64 tab_id) { send_simple("back"sv, tab_id); }
void EngineClient::go_forward(u64 tab_id) { send_simple("forward"sv, tab_id); }

void EngineClient::reload(u64 tab_id, bool bypass_cache)
{
    JsonObject event;
    event.set("type"sv, "reload"sv);
    event.set("tab"sv, tab_id);
    event.set("bypass_cache"sv, bypass_cache);
    send(event);
}

void EngineClient::stop(u64 tab_id) { send_simple("stop"sv, tab_id); }

void EngineClient::resize(u64 tab_id, Gfx::IntSize size)
{
    JsonObject event;
    event.set("type"sv, "resize"sv);
    event.set("tab"sv, tab_id);
    event.set("width"sv, size.width());
    event.set("height"sv, size.height());
    send(event);
}

void EngineClient::set_focus(u64 tab_id, bool focused)
{
    JsonObject event;
    event.set("type"sv, "focus"sv);
    event.set("tab"sv, tab_id);
    event.set("focused"sv, focused);
    send(event);
}


void EngineClient::mouse_event(u64 tab_id, StringView type, Gfx::IntPoint position, StringView button, unsigned buttons, unsigned modifiers, int click_count)
{
    JsonObject event;
    event.set("type"sv, "mouse"sv);
    event.set("tab"sv, tab_id);
    event.set("event"sv, type);
    event.set("x"sv, position.x());
    event.set("y"sv, position.y());
    event.set("button"sv, button);
    event.set("buttons"sv, buttons);
    event.set("ctrl"sv, (modifiers & Mod_Ctrl) != 0);
    event.set("shift"sv, (modifiers & Mod_Shift) != 0);
    event.set("alt"sv, (modifiers & Mod_Alt) != 0);
    event.set("super"sv, (modifiers & Mod_Super) != 0);
    event.set("click_count"sv, click_count);
    send(event);
}

void EngineClient::wheel_event(u64 tab_id, Gfx::IntPoint position, double delta_x, double delta_y, unsigned modifiers)
{
    JsonObject event;
    event.set("type"sv, "wheel"sv);
    event.set("tab"sv, tab_id);
    event.set("x"sv, position.x());
    event.set("y"sv, position.y());
    event.set("delta_x"sv, delta_x);
    event.set("delta_y"sv, delta_y);
    event.set("ctrl"sv, (modifiers & Mod_Ctrl) != 0);
    event.set("shift"sv, (modifiers & Mod_Shift) != 0);
    event.set("alt"sv, (modifiers & Mod_Alt) != 0);
    event.set("super"sv, (modifiers & Mod_Super) != 0);
    send(event);
}

void EngineClient::key_event(u64 tab_id, StringView type, StringView key_name, u32 code_point, unsigned modifiers, bool repeat)
{
    JsonObject event;
    event.set("type"sv, "key"sv);
    event.set("tab"sv, tab_id);
    event.set("event"sv, type);
    event.set("key"sv, key_name);
    event.set("codepoint"sv, code_point);
    event.set("ctrl"sv, (modifiers & Mod_Ctrl) != 0);
    event.set("shift"sv, (modifiers & Mod_Shift) != 0);
    event.set("alt"sv, (modifiers & Mod_Alt) != 0);
    event.set("super"sv, (modifiers & Mod_Super) != 0);
    event.set("repeat"sv, repeat);
    send(event);
}

void EngineClient::dialog_response(u64 tab_id, StringView kind, bool accept, String const& text)
{
    JsonObject event;
    event.set("type"sv, "dialog_response"sv);
    event.set("tab"sv, tab_id);
    event.set("kind"sv, kind);
    event.set("accept"sv, accept);
    event.set("text"sv, text.bytes_as_string_view());
    send(event);
}

void EngineClient::set_zoom(u64 tab_id, StringView direction)
{
    JsonObject event;
    event.set("type"sv, "zoom"sv);
    event.set("tab"sv, tab_id);
    event.set("direction"sv, direction);
    send(event);
}

void EngineClient::find_in_page(u64 tab_id, String const& query)
{
    JsonObject event;
    event.set("type"sv, "find"sv);
    event.set("tab"sv, tab_id);
    event.set("query"sv, query.bytes_as_string_view());
    send(event);
}

void EngineClient::find_next(u64 tab_id)
{
    send_simple("find_next"sv, tab_id);
}

void EngineClient::find_previous(u64 tab_id)
{
    send_simple("find_previous"sv, tab_id);
}

void EngineClient::toggle_bookmark(u64 tab_id)
{
    send_simple("toggle_bookmark"sv, tab_id);
}

void EngineClient::copy(u64 tab_id, bool cut)
{
    send_simple(cut ? "cut"sv : "copy"sv, tab_id);
}

void EngineClient::paste(u64 tab_id, StringView text)
{
    JsonObject event;
    event.set("type"sv, "paste"sv);
    event.set("tab"sv, tab_id);
    event.set("text"sv, text);
    send(event);
}

void EngineClient::select_all(u64 tab_id) { send_simple("select_all"sv, tab_id); }
void EngineClient::undo(u64 tab_id) { send_simple("undo"sv, tab_id); }
void EngineClient::redo(u64 tab_id) { send_simple("redo"sv, tab_id); }

void EngineClient::context_menu_action(u64 tab_id, u64 action)
{
    JsonObject event;
    event.set("type"sv, "context_menu_action"sv);
    event.set("tab"sv, tab_id);
    event.set("action"sv, action);
    send(event);
}

void EngineClient::permission_response(u64 tab_id, u64 request_id, bool granted)
{
    JsonObject event;
    event.set("type"sv, "permission_response"sv);
    event.set("tab"sv, tab_id);
    event.set("request"sv, request_id);
    event.set("granted"sv, granted);
    send(event);
}

void EngineClient::file_picker_response(u64 tab_id, String const& path)
{
    JsonObject event;
    event.set("type"sv, "file_picker_response"sv);
    event.set("tab"sv, tab_id);
    event.set("path"sv, path.bytes_as_string_view());
    send(event);
}

void EngineClient::select_dropdown_closed(u64 tab_id, Optional<u32> selected)
{
    JsonObject event;
    event.set("type"sv, "select_dropdown_closed"sv);
    event.set("tab"sv, tab_id);
    if (selected.has_value())
        event.set("selected"sv, static_cast<u64>(*selected));
    send(event);
}

void EngineClient::download_action(StringView action, u64 id)
{
    JsonObject event;
    event.set("type"sv, "download_action"sv);
    event.set("action"sv, action);
    event.set("id"sv, id);
    send(event);
}

void EngineClient::quit()
{
    if (m_quitting)
        return;
    m_quitting = true;
    JsonObject event;
    event.set("type"sv, "quit"sv);
    send(event);
}

}
