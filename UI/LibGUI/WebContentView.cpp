/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentView.h"

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

void WebContentView::paint_event(GUI::PaintEvent& event)
{
    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    if (m_frame) {
        if (m_frame->size() == size())
            painter.blit({ 0, 0 }, *m_frame, m_frame->rect());
        else
            painter.draw_scaled_bitmap(rect(), *m_frame, m_frame->rect());
    } else {
        painter.fill_rect(rect(), palette().color(Gfx::ColorRole::Base));
    }

    if (m_crashed) {
        painter.fill_rect(rect(), Gfx::Color::from_argb(0xa0202020));
        painter.draw_text(rect(), "The page crashed — click to reload"sv, font(), Gfx::TextAlignment::Center, Gfx::Color::from_rgb(0xffffff));
    }
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

    // Let command modifiers and registered window shortcuts (F5, F11, ...)
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
