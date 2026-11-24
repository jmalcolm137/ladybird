/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineServer.h"
#include "EngineApplication.h"

#include <AK/Checked.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibWebView/Application.h>
#include <LibWebView/ProcessManager.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace LadybirdSDE {

static bool trace_frames()
{
    static bool const enabled = getenv("LADYBIRD_SDE_TRACE_FRAMES") != nullptr;
    return enabled;
}

void EngineServer::destroy_frame_buffer(Tab& tab)
{
    if (tab.frame_mapping) {
        (void)::munmap(tab.frame_mapping, tab.frame_mapping_size);
        tab.frame_mapping = nullptr;
        tab.frame_mapping_size = 0;
    }
    if (tab.frame_fd >= 0) {
        ::close(tab.frame_fd);
        tab.frame_fd = -1;
    }
    if (!tab.frame_buffer_name.is_empty()) {
        ::shm_unlink(tab.frame_buffer_name.characters());
        tab.frame_buffer_name = {};
    }
    tab.frame_slot_size = 0;
    tab.frame_buffer_dimensions = {};
    tab.frame_write_slot = 0;
    reset_frame_tracking(tab);
}

bool EngineServer::ensure_frame_buffer(Tab& tab, Gfx::IntSize dimensions)
{
    auto width = dimensions.width();
    auto height = dimensions.height();
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return false;

    Checked<size_t> checked_slot_size = static_cast<size_t>(width);
    checked_slot_size *= static_cast<size_t>(height);
    checked_slot_size *= 4;
    if (checked_slot_size.has_overflow())
        return false;
    auto slot_size = checked_slot_size.value();
    if (tab.frame_mapping && tab.frame_slot_size >= slot_size && tab.frame_buffer_dimensions == dimensions)
        return true;

    Checked<size_t> checked_total_bytes = slot_size;
    checked_total_bytes *= static_cast<size_t>(tab.frame_slots);
    if (checked_total_bytes.has_overflow() || checked_total_bytes.value() > 256 * MiB)
        return false;
    auto bytes = checked_total_bytes.value();

    // Recreate on a size change even when the old slots are large enough: the
    // in-flight slots still contain the old dimensions, and their acks refer to
    // a layout the frontend can no longer use safely.
    destroy_frame_buffer(tab);

    tab.frame_slot_size = slot_size;
    tab.frame_buffer_dimensions = dimensions;

    auto name = ByteString::formatted("/ladybird-sde-{}-{}-{}", ::getpid(), tab.id, m_next_frame_buffer_id++);
    int fd = ::shm_open(name.characters(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        warnln("LadybirdEngine: could not create frame buffer '{}': {}", name, strerror(errno));
        tab.frame_slot_size = 0;
        return false;
    }

    if (::ftruncate(fd, static_cast<off_t>(bytes)) < 0) {
        warnln("LadybirdEngine: could not resize frame buffer: {}", strerror(errno));
        ::close(fd);
        ::shm_unlink(name.characters());
        tab.frame_slot_size = 0;
        return false;
    }

    auto* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        warnln("LadybirdEngine: could not map frame buffer: {}", strerror(errno));
        ::close(fd);
        ::shm_unlink(name.characters());
        tab.frame_slot_size = 0;
        return false;
    }

    tab.frame_fd = fd;
    tab.frame_mapping = mapping;
    tab.frame_mapping_size = bytes;
    tab.frame_buffer_name = name;
    tab.frame_write_slot = 0;

    JsonObject event;
    event.set("type"sv, "frame_buffer"sv);
    event.set("tab"sv, tab.id);
    event.set("name"sv, tab.frame_buffer_name.view());
    event.set("slots"sv, tab.frame_slots);
    event.set("slot_size"sv, static_cast<u64>(tab.frame_slot_size));
    event.set("width"sv, width);
    event.set("height"sv, height);
    event.set("scale"sv, tab.device_pixel_ratio);
    send_event(move(event));
    return true;
}


void EngineServer::reset_frame_tracking(Tab& tab)
{
    tab.frame_slot_states.clear();
    tab.frame_slot_states.ensure_capacity(tab.frame_slots);
    for (int i = 0; i < tab.frame_slots; ++i)
        tab.frame_slot_states.append({});
    tab.frame_damage_history.clear();
    tab.frame_content_size = {};
    tab.frame_write_slot = 0;
}

void EngineServer::on_view_painted(Tab& tab)
{
    if (tab.view->zoom_level() != tab.last_reported_zoom) {
        tab.last_reported_zoom = tab.view->zoom_level();
        send_zoom_changed(tab);
    }

    ++tab.presented_sequence;
    tab.frame_damage_history.append({ tab.presented_sequence, tab.view->last_presented_damage_rect() });
    if (tab.frame_damage_history.size() > 512)
        tab.frame_damage_history.remove(0, tab.frame_damage_history.size() - 512);
    tab.frame_dirty = true;
    try_send_frame(tab);
}

Gfx::IntRect EngineServer::damage_since(Tab& tab, u64 content_sequence, u64 latest_sequence, Gfx::IntSize size, bool& needs_full_copy)
{
    needs_full_copy = false;
    auto frame_rect = Gfx::IntRect { 0, 0, size.width(), size.height() };
    if (content_sequence >= latest_sequence)
        return {};
    if (content_sequence == 0) {
        needs_full_copy = true;
        return frame_rect;
    }

    bool found_damage = false;
    bool found_first = false;
    u64 first_sequence = 0;
    Gfx::IntRect union_rect;
    for (auto const& damage : tab.frame_damage_history) {
        if (damage.sequence <= content_sequence)
            continue;
        if (damage.sequence > latest_sequence)
            break;
        if (!found_first) {
            first_sequence = damage.sequence;
            found_first = true;
        }
        found_damage = true;
        auto rect = damage.rect.intersected(frame_rect);
        if (rect.is_empty())
            continue;
        union_rect = union_rect.is_empty() ? rect : union_rect.united(rect);
    }

    if (!found_damage || first_sequence > content_sequence + 1) {
        // The slot is older than the damage history we retained, so we cannot
        // know every changed pixel. Fall back to a full copy.
        needs_full_copy = true;
        return frame_rect;
    }

    if (union_rect.is_empty())
        return union_rect;

    auto frame_area = static_cast<u64>(size.width()) * static_cast<u64>(size.height());
    auto union_area = static_cast<u64>(union_rect.width()) * static_cast<u64>(union_rect.height());
    if (frame_area != 0 && union_area * 5 > frame_area * 3) {
        needs_full_copy = true;
        return frame_rect;
    }
    return union_rect;
}

void EngineServer::try_send_frame(Tab& tab)
{
    if (!tab.frame_dirty)
        return;

    auto bitmap = tab.view->displayed_bitmap();
    if (!bitmap)
        return;

    auto size = bitmap->size();
    if (size.is_empty())
        return;

    if (tab.last_reported_frame_size != size) {
        if (trace_frames())
            warnln("LadybirdEngine: tab {} viewport {}x{} frame {}x{} zoom {}", tab.id, tab.viewport_size.width(), tab.viewport_size.height(), size.width(), size.height(), tab.view->zoom_level());
        tab.last_reported_frame_size = size;
    }

    // A painted frame can briefly be larger than the requested viewport during a
    // resize. Size the ring for the frame, but do not feed the frame size back
    // into the requested viewport: doing so makes the host oscillate between the
    // old and new size and the frontend scales frames up and down.
    if (!ensure_frame_buffer(tab, size))
        return;

    if (tab.frame_content_size != size) {
        reset_frame_tracking(tab);
        tab.frame_content_size = size;
    }
    if (tab.frame_slot_states.size() != static_cast<size_t>(tab.frame_slots))
        reset_frame_tracking(tab);

    int slot = -1;
    u64 best_content_sequence = 0;
    for (int i = 0; i < tab.frame_slots; ++i) {
        auto const& state = tab.frame_slot_states[i];
        if (state.in_flight)
            continue;
        if (slot < 0 || state.content_sequence > best_content_sequence) {
            slot = i;
            best_content_sequence = state.content_sequence;
        }
    }
    if (slot < 0) {
        // All slots are still owned by the frontend. Keep frame_dirty set; the
        // next ack will publish the latest frame and intermediate frames are
        // coalesced instead of overwriting pixels in use.
        return;
    }

    auto& slot_state = tab.frame_slot_states[slot];
    bool full_copy = slot_state.content_sequence == 0;
    bool needs_full_copy = false;
    auto damage = Gfx::IntRect { 0, 0, size.width(), size.height() };
    if (!full_copy) {
        damage = damage_since(tab, slot_state.content_sequence, tab.presented_sequence, size, needs_full_copy);
        full_copy = needs_full_copy;
    }

    if (!full_copy && damage.is_empty()) {
        slot_state.content_sequence = tab.presented_sequence;
        tab.frame_dirty = false;
        return;
    }

    auto* destination = static_cast<u8*>(tab.frame_mapping) + (static_cast<size_t>(slot) * tab.frame_slot_size);
    auto stride = static_cast<size_t>(size.width()) * 4;

    if (full_copy) {
        damage = { 0, 0, size.width(), size.height() };
        for (int y = 0; y < size.height(); ++y)
            memcpy(destination + (static_cast<size_t>(y) * stride), bitmap->scanline_u8(y), stride);
    } else {
        for (int y = damage.y(); y < damage.bottom(); ++y) {
            memcpy(destination + (static_cast<size_t>(y) * stride) + static_cast<size_t>(damage.x()) * 4,
                bitmap->scanline_u8(y) + static_cast<size_t>(damage.x()) * 4,
                static_cast<size_t>(damage.width()) * 4);
        }
    }

    slot_state.content_sequence = tab.presented_sequence;
    slot_state.in_flight_sequence = tab.presented_sequence;
    slot_state.in_flight = true;
    tab.frame_write_slot = (slot + 1) % tab.frame_slots;
    tab.frame_dirty = false;

    if (trace_frames())
        warnln("LadybirdEngine: tab {} frame {} slot {} full={} damage {}x{}+{},{}", tab.id, tab.presented_sequence, slot, full_copy, damage.width(), damage.height(), damage.x(), damage.y());

    JsonObject event;
    event.set("type"sv, "frame"sv);
    event.set("tab"sv, tab.id);
    event.set("slot"sv, slot);
    event.set("width"sv, size.width());
    event.set("height"sv, size.height());
    event.set("stride"sv, static_cast<u64>(stride));
    event.set("scale"sv, tab.device_pixel_ratio);
    event.set("sequence"sv, tab.presented_sequence);
    event.set("full"sv, full_copy);
    event.set("damage_x"sv, damage.x());
    event.set("damage_y"sv, damage.y());
    event.set("damage_width"sv, damage.width());
    event.set("damage_height"sv, damage.height());
    send_event(move(event));
    note_frame_presented(tab, tab.presented_sequence);
}

void EngineServer::note_user_input(Tab& tab)
{
    ++tab.input_events_since_frame;
    if (tab.waiting_for_frame)
        return;
    if (trace_frames())
        warnln("LadybirdEngine: tab {} waiting for frame after input", tab.id);
    tab.waiting_for_frame = true;
    tab.input_wait_started = MonotonicTime::now();
    tab.last_cpu_sample_time = tab.input_wait_started;
    tab.last_cpu_ticks = web_content_cpu_ticks(tab.view->web_content_pid()).value_or(0);
    ensure_maintenance_timer_running();
}

void EngineServer::note_frame_presented(Tab& tab, u64 sequence)
{
    (void)sequence;
    tab.has_presented_frame = true;
    tab.input_events_since_frame = 0;
    tab.wheel_events_since_frame = 0;

    if (tab.unresponsive_reported) {
        // A frame that was already queued when the renderer spun can arrive
        // after the watchdog fires. Require two subsequent frames before
        // declaring the page responsive again; a genuinely recovered page will
        // paint again, while a single stale frame will not clear the banner.
        if (++tab.responsive_frame_count < 2)
            return;
        tab.unresponsive_reported = false;
        tab.responsive_frame_count = 0;
        set_unresponsive(tab, false);
    }

    tab.waiting_for_frame = false;
}

Optional<u64> EngineServer::web_content_cpu_ticks(pid_t pid)
{
    if (pid <= 0)
        return {};
    auto file = Core::File::open(ByteString::formatted("/proc/{}/stat", pid), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    auto buffer = contents.release_value();
    StringView text { reinterpret_cast<char const*>(buffer.data()), buffer.size() };
    Optional<size_t> close_paren;
    for (size_t i = text.length(); i > 0; --i) {
        if (text[i - 1] == ')') {
            close_paren = i - 1;
            break;
        }
    }
    if (!close_paren.has_value())
        return {};
    auto fields = text.substring_view(*close_paren + 1).trim_whitespace().split_view(' ');
    // After the comm field, fields[0] is state and utime/stime are fields 14 and 15.
    if (fields.size() < 13)
        return {};
    auto utime = fields[11].to_number<u64>();
    auto stime = fields[12].to_number<u64>();
    if (!utime.has_value() || !stime.has_value())
        return {};
    return *utime + *stime;
}

void EngineServer::snapshot_resolver_files()
{
    static constexpr Array<StringView, 4> resolver_paths {
        "/etc/resolv.conf"sv,
        "/etc/hosts"sv,
        "/etc/nsswitch.conf"sv,
        "/etc/host.conf"sv,
    };

    Vector<StringView> paths;
    if (auto configured = Core::Environment::get("LADYBIRD_SDE_RESOLVER_FILES"sv); configured.has_value() && !configured->is_empty()) {
        for (auto path : configured->split_view(':'))
            paths.append(path);
    } else {
        for (auto path : resolver_paths)
            paths.append(path);
    }

    m_resolver_files.clear();
    m_resolver_files.ensure_capacity(paths.size());

    for (auto path : paths) {
        ResolverFileState state;
        state.path = path.to_byte_string();

        if (auto result = Core::System::stat(path); !result.is_error()) {
            auto const& metadata = result.value();
            state.exists = true;
            state.device = metadata.st_dev;
            state.inode = metadata.st_ino;
            state.mtime = metadata.st_mtime;
            state.size = metadata.st_size;
        }

        m_resolver_files.append(move(state));
    }
}

void EngineServer::check_resolver_files()
{
    if (m_quitting)
        return;

    bool changed = false;
    Vector<ResolverFileState> current_states;
    current_states.ensure_capacity(m_resolver_files.size());

    for (auto const& previous : m_resolver_files) {
        ResolverFileState current;
        current.path = previous.path;

        if (auto result = Core::System::stat(previous.path.view()); !result.is_error()) {
            auto const& metadata = result.value();
            current.exists = true;
            current.device = metadata.st_dev;
            current.inode = metadata.st_ino;
            current.mtime = metadata.st_mtime;
            current.size = metadata.st_size;
        }

        if (current.exists != previous.exists
            || current.device != previous.device
            || current.inode != previous.inode
            || current.mtime != previous.mtime
            || current.size != previous.size) {
            changed = true;
        }

        current_states.append(move(current));
    }

    m_resolver_files = move(current_states);

    if (!changed)
        return;

    warnln("LadybirdEngine: resolver configuration changed; restarting RequestServers");
    auto& process_manager = WebView::Application::process_manager();
    Vector<u64> request_server_ids;
    process_manager.for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::RequestServer)
            request_server_ids.append(process.child_id());
    });
    for (auto child_id : request_server_ids)
        (void)process_manager.terminate_child(child_id, Core::Process::TerminationMode::Forceful);
}

void EngineServer::ensure_maintenance_timer_running()
{
    if (m_quitting || !m_unresponsive_timer)
        return;
    if (!m_unresponsive_timer->is_active())
        m_unresponsive_timer->start();
}


void EngineServer::release_idle_hidden_frame_buffers()
{
    static i64 const release_delay_ms = [] {
        if (auto configured = Core::Environment::get("LADYBIRD_SDE_HIDDEN_SHM_RELEASE_MS"sv); configured.has_value()) {
            if (auto parsed = configured->to_number<i64>(); parsed.has_value() && *parsed >= 0)
                return *parsed;
        }
        return static_cast<i64>(10000);
    }();

    auto now = MonotonicTime::now();
    for (auto& [id, tab] : m_tabs) {
        (void)id;
        if (!tab->frame_mapping || !tab->hidden_since.has_value())
            continue;
        if ((now - *tab->hidden_since).to_milliseconds() < release_delay_ms)
            continue;

        destroy_frame_buffer(*tab);
        tab->frame_dirty = true;
        send_simple_event("frame_buffer_release"sv, tab->id);
    }
}

void EngineServer::check_unresponsive_tabs()
{
    auto now = MonotonicTime::now();
    static auto const clock_ticks_per_second = static_cast<double>(::sysconf(_SC_CLK_TCK));
    bool any_waiting = false;
    bool any_hidden_shm = false;
    for (auto& [id, tab] : m_tabs) {
        (void)id;
        if (tab->hidden_since.has_value() && tab->frame_mapping)
            any_hidden_shm = true;
        if (!tab->waiting_for_frame || !tab->has_presented_frame || tab->unresponsive_reported)
            continue;
        any_waiting = true;

        auto waited_ms = (now - tab->input_wait_started).to_milliseconds();
        if (waited_ms < 1500)
            continue;

        auto ticks = web_content_cpu_ticks(tab->view->web_content_pid());
        if (!ticks.has_value())
            continue;

        auto sample_ms = (now - tab->last_cpu_sample_time).to_milliseconds();
        auto previous_ticks = tab->last_cpu_ticks;
        tab->last_cpu_sample_time = now;
        tab->last_cpu_ticks = *ticks;

        if (sample_ms <= 0 || previous_ticks == 0 || clock_ticks_per_second <= 0)
            continue;

        auto used_ticks = *ticks >= previous_ticks ? *ticks - previous_ticks : 0;
        auto cpu_fraction = (static_cast<double>(used_ticks) / clock_ticks_per_second) * 1000.0 / static_cast<double>(sample_ms);
        if (trace_frames())
            warnln("LadybirdEngine: tab {} unresponsive check waited={}ms cpu={}% ticks={}", tab->id, waited_ms, static_cast<int>(cpu_fraction * 100), *ticks);
        if (waited_ms >= 2500 && cpu_fraction >= 0.5) {
            tab->unresponsive_reported = true;
            tab->responsive_frame_count = 0;
            set_unresponsive(*tab, true);
        } else if (waited_ms >= 12000 && tab->wheel_events_since_frame >= 4 && cpu_fraction < 0.5) {
            // A renderer can also deadlock without spinning. Input is queued but
            // no frame arrives; after several attempts to move the page, offer a
            // hard reload rather than leaving the client area frozen.
            tab->unresponsive_reported = true;
            tab->responsive_frame_count = 0;
            set_unresponsive(*tab, true);
        }
    }

    if (any_hidden_shm) {
        release_idle_hidden_frame_buffers();
        any_hidden_shm = false;
        for (auto const& [id, tab] : m_tabs) {
            (void)id;
            if (tab->hidden_since.has_value() && tab->frame_mapping) {
                any_hidden_shm = true;
                break;
            }
        }
    }

    if (!any_waiting && !any_hidden_shm && m_unresponsive_timer)
        m_unresponsive_timer->stop();
}

void EngineServer::set_unresponsive(Tab& tab, bool unresponsive)
{
    if (trace_frames())
        warnln("LadybirdEngine: tab {} unresponsive={}", tab.id, unresponsive);
    JsonObject event;
    event.set("type"sv, "tab_unresponsive"sv);
    event.set("tab"sv, tab.id);
    event.set("unresponsive"sv, unresponsive);
    send_event(move(event));
}

void EngineServer::handle_frame_ack(u64 tab_id, JsonObject const& command)
{
    auto* tab = tab_for_id(tab_id);
    if (!tab)
        return;

    auto sequence = command.get_u64("sequence"sv);
    if (!sequence.has_value())
        return;

    for (auto& slot : tab->frame_slot_states) {
        if (slot.in_flight && slot.in_flight_sequence == *sequence) {
            slot.in_flight = false;
            slot.in_flight_sequence = 0;
        }
    }

    if (tab->frame_dirty)
        try_send_frame(*tab);
}


}
