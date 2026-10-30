/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGUI/Widget.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace LadybirdSDE {

// The page surface. It paints the frame the engine host publishes and turns
// LibGUI input into the frontend's small platform-event vocabulary.
class WebContentView final : public GUI::Widget {
    C_OBJECT(WebContentView)

public:
    virtual ~WebContentView() override = default;

    void set_tab_id(u64 tab_id) { m_tab_id = tab_id; }
    u64 tab_id() const { return m_tab_id; }

    void set_frame(Gfx::Bitmap const* frame, u64 sequence, Gfx::IntRect damage, bool full);

    void set_page_cursor(StringView cursor_name);
    void set_crashed(bool crashed) { m_crashed = crashed; update(); }
    bool is_crashed() const { return m_crashed; }
    void set_unresponsive(bool unresponsive) { m_unresponsive = unresponsive; update(); }
    bool is_unresponsive() const { return m_unresponsive; }

    Function<void(Gfx::IntSize)> on_resize;
    Function<void()> on_reload;
    Function<void(StringView type, Gfx::IntPoint position, GUI::MouseButton button, unsigned buttons, unsigned modifiers, int click_count)> on_mouse;
    Function<void(StringView type, Gfx::IntPoint position, unsigned modifiers, String const& text, Vector<String> const& urls)> on_drag;
    Function<void(Gfx::IntPoint position, int delta_x, int delta_y, unsigned modifiers)> on_wheel;
    Function<void(StringView type, StringView key_name, u32 code_point, unsigned modifiers)> on_key;
    Function<void(bool focused)> on_focus_change;
    Function<void()> on_go_back;
    Function<void()> on_go_forward;
    Function<void()> on_force_reload;
    Function<void(u64 sequence)> on_frame_ack;

protected:
    virtual void paint_event(GUI::PaintEvent&) override;
    virtual void resize_event(GUI::ResizeEvent&) override;
    virtual void drag_enter_event(GUI::DragEvent&) override;
    virtual void drag_move_event(GUI::DragEvent&) override;
    virtual void drag_leave_event(GUI::Event&) override;
    virtual void drop_event(GUI::DropEvent&) override;
    virtual void mousedown_event(GUI::MouseEvent&) override;
    virtual void mouseup_event(GUI::MouseEvent&) override;
    virtual void mousemove_event(GUI::MouseEvent&) override;
    virtual void doubleclick_event(GUI::MouseEvent&) override;
    virtual void mousewheel_event(GUI::MouseEvent&) override;
    virtual void leave_event(Core::Event&) override;
    virtual void keydown_event(GUI::KeyEvent&) override;
    virtual void keyup_event(GUI::KeyEvent&) override;
    virtual void focusin_event(GUI::FocusEvent&) override;
    virtual void focusout_event(GUI::FocusEvent&) override;

private:
    WebContentView();

    u64 m_tab_id { 0 };
    Gfx::Bitmap const* m_frame { nullptr };
    u64 m_frame_sequence { 0 };
    bool m_frame_was_painted { false };
    bool m_dragging { false };
    Gfx::IntPoint m_drag_position;
    int m_click_count { 1 };
    bool m_crashed { false };
    bool m_unresponsive { false };
};

}
