/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentView.h"

#include <LibCore/MimeData.h>
#include <LibGUI/Event.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Shortcut.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Palette.h>
#include <string.h>

namespace LadybirdSDE {

WebContentView::WebContentView()
{
    set_focus_policy(GUI::FocusPolicy::StrongFocus);
    set_override_cursor(Gfx::StandardCursor::Arrow);
}

void WebContentView::set_frame(Gfx::Bitmap const* frame, u64 sequence, Gfx::IntRect damage, bool full)
{
    // Replacing the frame releases the old shared-memory slot. The engine must
    // not reuse a slot while this widget can still paint from it.
    bool previous_was_painted = m_frame_was_painted;
    if (m_frame && m_frame_sequence != 0 && m_frame_sequence != sequence) {
        if (on_frame_ack)
            on_frame_ack(m_frame_sequence);
    }

    m_frame = frame;
    m_frame_sequence = frame ? sequence : 0;
    m_frame_was_painted = false;

    if (!m_frame) {
        update();
        return;
    }

    // If the previous frame was replaced before it was painted, the widget's
    // backing store is older than the damage rectangle describes. Repaint the
    // whole frame in that case.
    if (full || !previous_was_painted || damage.is_empty() || m_frame->size() != size())
        update();
    else
        update(damage.intersected(rect()));
}

RefPtr<Gfx::Bitmap> WebContentView::snapshot() const
{
    if (!m_frame)
        return {};
    auto cloned = m_frame->clone();
    if (cloned.is_error())
        return {};
    return cloned.release_value();
}

void WebContentView::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    if (m_frame) {
        if (m_frame->size() == size()) {
            painter.blit({ 0, 0 }, *m_frame, m_frame->rect());
        } else {
            // A frame can briefly be a different size while the engine catches up
            // with a window resize. Scaling it to fit makes the page text grow and
            // shrink, which reads as the page zooming for no reason. Draw it at
            // its native size instead (cropping or leaving an edge blank for a
            // frame); the next frame will match the view again.
            static Gfx::IntSize last_frame_size;
            static Gfx::IntSize last_widget_size;
            if (m_frame->size() != last_frame_size || size() != last_widget_size) {
                warnln("Ladybird: painting a {}x{} frame into a {}x{} view without scaling", m_frame->size().width(), m_frame->size().height(), size().width(), size().height());
                last_frame_size = m_frame->size();
                last_widget_size = size();
            }
            painter.fill_rect(rect(), palette().color(Gfx::ColorRole::Base));
            painter.blit({ 0, 0 }, *m_frame, m_frame->rect().intersected(rect()));
        }
    } else {
        painter.fill_rect(rect(), palette().color(Gfx::ColorRole::Base));
    }

    if (m_crashed) {
        painter.fill_rect(rect(), Gfx::Color::from_argb(0xa0202020));
        painter.draw_text(rect(), "The page crashed — click to reload"sv, font(), Gfx::TextAlignment::Center, Gfx::Color::from_rgb(0xffffff));
    } else if (m_unresponsive) {
        Gfx::IntRect banner { 0, 0, width(), 34 };
        painter.fill_rect(banner, Gfx::Color::from_argb(0xdd202020));
        painter.draw_text(banner, "Page unresponsive — click to reload"sv, font(), Gfx::TextAlignment::Center, Gfx::Color::from_rgb(0xffffff));
    }

    m_frame_was_painted = m_frame != nullptr;
}

void WebContentView::drag_enter_event(GUI::DragEvent& event)
{
    m_dragging = true;
    m_drag_position = event.position();
    event.accept();
    if (!on_drag)
        return;
    auto text_data = event.mime_data().data("text/plain;charset=utf-8"sv);
    if (text_data.is_empty())
        text_data = event.mime_data().data("text/plain"sv);
    auto text = String::from_utf8_with_replacement_character(StringView { text_data.bytes() });
    Vector<String> urls;
    for (auto const& url : event.mime_data().urls())
        urls.append(String::from_byte_string(url.serialize()).release_value());
    on_drag("start"sv, event.position(), event.modifiers(), text, urls);
}

void WebContentView::drag_move_event(GUI::DragEvent& event)
{
    if (!m_dragging) {
        drag_enter_event(event);
        return;
    }
    m_drag_position = event.position();
    if (on_drag)
        on_drag("move"sv, event.position(), event.modifiers(), {}, {});
}

void WebContentView::drag_leave_event(GUI::Event&)
{
    if (m_dragging && on_drag)
        on_drag("end"sv, m_drag_position, 0, {}, {});
    m_dragging = false;
}

void WebContentView::drop_event(GUI::DropEvent& event)
{
    m_dragging = false;
    event.accept();
    if (!on_drag)
        return;
    auto text_data = event.mime_data().data("text/plain;charset=utf-8"sv);
    if (text_data.is_empty())
        text_data = event.mime_data().data("text/plain"sv);
    auto text = String::from_utf8_with_replacement_character(StringView { text_data.bytes() });
    Vector<String> urls;
    for (auto const& url : event.mime_data().urls())
        urls.append(String::from_byte_string(url.serialize()).release_value());
    on_drag("drop"sv, event.position(), event.modifiers(), text, urls);
}

void WebContentView::resize_event(GUI::ResizeEvent& event)
{
    GUI::Widget::resize_event(event);
    if (on_resize)
        on_resize(event.size());
}

void WebContentView::mousedown_event(GUI::MouseEvent& event)
{
    set_focus(true);

    if (m_unresponsive && event.position().y() < 34) {
        if (on_force_reload)
            on_force_reload();
        event.accept();
        return;
    }

    if (m_crashed) {
        if (on_reload)
            on_reload();
        event.accept();
        return;
    }

    if (on_mouse)
        on_mouse("down"sv, event.position(), event.button(), event.buttons(), event.modifiers(), m_click_count);

    if (event.button() == GUI::MouseButton::Backward) {
        if (on_go_back)
            on_go_back();
    } else if (event.button() == GUI::MouseButton::Forward) {
        if (on_go_forward)
            on_go_forward();
    }

    event.accept();
}

void WebContentView::mouseup_event(GUI::MouseEvent& event)
{
    if (on_mouse)
        on_mouse("up"sv, event.position(), event.button(), event.buttons(), event.modifiers(), m_click_count);
    event.accept();
}

void WebContentView::mousemove_event(GUI::MouseEvent& event)
{
    if (on_mouse)
        on_mouse("move"sv, event.position(), event.button(), event.buttons(), event.modifiers(), 0);
    event.accept();
}

void WebContentView::doubleclick_event(GUI::MouseEvent& event)
{
    if (on_mouse)
        on_mouse("down"sv, event.position(), event.button(), event.buttons(), event.modifiers(), 2);
    event.accept();
}

void WebContentView::mousewheel_event(GUI::MouseEvent& event)
{
    // LibGUI reports wheel motion in notches; the engine wants pixel deltas.
    static constexpr int scroll_step_size = 24;
    if (on_wheel)
        on_wheel(event.position(), event.wheel_delta_x() * scroll_step_size, event.wheel_delta_y() * scroll_step_size, event.modifiers());
    event.accept();
}

void WebContentView::leave_event(Core::Event& event)
{
    if (on_mouse)
        on_mouse("leave"sv, {}, GUI::MouseButton::None, 0, 0, 0);
    GUI::Widget::leave_event(event);
}

void WebContentView::keydown_event(GUI::KeyEvent& event)
{
    auto* key_name = key_code_to_string(event.key());
    if (on_key) {
        auto key_view = key_name ? StringView { key_name, strlen(key_name) } : StringView { ""sv };
        on_key("down"sv, key_view, event.code_point(), event.modifiers());
    }

    // Editing shortcuts belong to the page while it has focus, so WebCore runs its own copy/paste path with the
    // full set of clipboard representations. Letting the window actions fire as well would copy the same selection
    // twice, once as plain text.
    auto key = event.key();
    auto page_edits = event.ctrl() && !event.alt() && !event.super()
        && (key == Key_X || key == Key_V || key == Key_A || key == Key_Z || key == Key_Y
            || (key == Key_C && !event.shift()));
    if (page_edits) {
        event.accept();
        return;
    }

    // Let other command modifiers and registered window shortcuts (F5, F11, ...)
    // reach the window's actions. A plain key press belongs to the page.
    auto* action = window() ? window()->action_for_shortcut(GUI::Shortcut(static_cast<u8>(event.modifiers()), event.key())) : nullptr;
    if (event.ctrl() || event.alt() || event.super() || action != nullptr)
        event.ignore();
    else
        event.accept();
}

void WebContentView::keyup_event(GUI::KeyEvent& event)
{
    auto* key_name = key_code_to_string(event.key());
    if (on_key) {
        auto key_view = key_name ? StringView { key_name, strlen(key_name) } : StringView { ""sv };
        on_key("up"sv, key_view, event.code_point(), event.modifiers());
    }
    event.accept();
}

void WebContentView::focusin_event(GUI::FocusEvent& event)
{
    if (on_focus_change)
        on_focus_change(true);
    GUI::Widget::focusin_event(event);
}

void WebContentView::focusout_event(GUI::FocusEvent& event)
{
    if (on_focus_change)
        on_focus_change(false);
    GUI::Widget::focusout_event(event);
}

void WebContentView::set_page_cursor(StringView cursor_name)
{
    auto cursor = Gfx::StandardCursor::Arrow;
    if (cursor_name == "IBeam"sv)
        cursor = Gfx::StandardCursor::IBeam;
    else if (cursor_name == "Hand"sv || cursor_name == "OpenHand"sv)
        cursor = Gfx::StandardCursor::Hand;
    else if (cursor_name == "Crosshair"sv)
        cursor = Gfx::StandardCursor::Crosshair;
    else if (cursor_name == "ResizeHorizontal"sv || cursor_name == "ResizeColumn"sv)
        cursor = Gfx::StandardCursor::ResizeHorizontal;
    else if (cursor_name == "ResizeVertical"sv || cursor_name == "ResizeRow"sv)
        cursor = Gfx::StandardCursor::ResizeVertical;
    else if (cursor_name == "ResizeDiagonalTLBR"sv)
        cursor = Gfx::StandardCursor::ResizeDiagonalTLBR;
    else if (cursor_name == "ResizeDiagonalBLTR"sv)
        cursor = Gfx::StandardCursor::ResizeDiagonalBLTR;
    else if (cursor_name == "Wait"sv)
        cursor = Gfx::StandardCursor::Wait;
    else if (cursor_name == "Help"sv)
        cursor = Gfx::StandardCursor::Help;
    else if (cursor_name == "Move"sv)
        cursor = Gfx::StandardCursor::Move;
    else if (cursor_name == "Drag"sv)
        cursor = Gfx::StandardCursor::Drag;
    else if (cursor_name == "DragCopy"sv)
        cursor = Gfx::StandardCursor::DragCopy;
    else if (cursor_name == "Disallowed"sv)
        cursor = Gfx::StandardCursor::Disallowed;
    else if (cursor_name == "Eyedropper"sv)
        cursor = Gfx::StandardCursor::Eyedropper;
    else if (cursor_name == "Zoom"sv)
        cursor = Gfx::StandardCursor::Zoom;

    set_override_cursor(cursor);
}

}
