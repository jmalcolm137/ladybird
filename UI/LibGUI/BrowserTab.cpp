/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserTab.h"

#include <LibCore/StandardPaths.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/JsonArrayModel.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/TableView.h>
#include <LibGUI/ToolbarContainer.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>

namespace LadybirdSDE {

class MenuButton final : public GUI::Button {
    C_OBJECT(MenuButton);
public:
    Function<void()> on_menu_request;
    virtual ~MenuButton() override = default;

private:
    explicit MenuButton(String text)
        : Button(move(text))
    {
    }

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        if (on_menu_request) {
            on_menu_request();
            event.accept();
            return;
        }
        Button::mousedown_event(event);
    }
};

struct SiteIdentity {
    String security;
    String host;
};

static SiteIdentity site_identity_for_url(String const& url)
{
    SiteIdentity identity { "Site information"_string, {} };

    auto url_view = url.bytes_as_string_view();
    if (url_view.starts_with("https:"sv))
        identity.security = "Secure connection"_string;
    else if (url_view.starts_with("http:"sv))
        identity.security = "Not secure"_string;
    else if (url_view.starts_with("file:"sv))
        identity.security = "Local file"_string;
    else if (url_view.starts_with("about:"sv))
        identity.security = "Browser page"_string;
    else if (url_view.starts_with("view-source:"sv))
        identity.security = "View source"_string;

    auto view = url_view;
    auto scheme_separator = view.find("://"sv);
    if (scheme_separator.has_value()) {
        auto rest = view.substring_view(*scheme_separator + 3);
        size_t end = 0;
        while (end < rest.length() && rest[end] != '/' && rest[end] != '?' && rest[end] != '#')
            ++end;
        if (end > 0)
            identity.host = String::from_utf8(rest.substring_view(0, end)).release_value();
    }

    return identity;
}

BrowserTab::BrowserTab(EngineClient& engine, GUI::Window& window, GUI::Action& back_action, GUI::Action& forward_action, GUI::Action& reload_action, GUI::Action& stop_action, GUI::Action& bookmark_action, GUI::Action& home_action, GUI::Action& new_tab_action, GUI::Action& downloads_action)
    : m_engine(engine)
    , m_root(GUI::Widget::construct())
{
    m_title = "New Tab"_string;

    m_root->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 0 }, 0);

    auto& toolbar_container = m_root->add<GUI::ToolbarContainer>();
    m_toolbar = toolbar_container.add<GUI::Toolbar>();

    m_toolbar->add_action(back_action);
    m_toolbar->add_action(forward_action);
    m_toolbar->add_action(reload_action);
    m_toolbar->add_action(stop_action);
    m_toolbar->add_action(bookmark_action);
    m_toolbar->add_action(home_action);
    m_toolbar->add_action(new_tab_action);
    m_toolbar->add_action(downloads_action);
    m_toolbar->add_separator();

    auto& site_button = m_toolbar->add<MenuButton>("Site"_string);
    site_button.set_button_style(Gfx::ButtonStyle::Coolbar);
    site_button.set_fixed_height(22);
    site_button.set_fixed_width(76);
    site_button.on_menu_request = [this, &site_button] {
        if (m_site_menu)
            m_site_menu->popup(site_button.screen_relative_rect().bottom_left(), {}, site_button.rect());
    };
    m_site_button = site_button;

    m_bookmarks_bar = m_root->add<GUI::Widget>();
    m_bookmarks_bar->set_fixed_height(28);
    m_bookmarks_bar->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 4 }, 4);
    m_bookmarks_bar->set_visible(false);

    auto& tabs_button = m_toolbar->add<MenuButton>("Tabs"_string);
    tabs_button.set_button_style(Gfx::ButtonStyle::Coolbar);
    tabs_button.set_fixed_height(22);
    tabs_button.set_fixed_width(44);
    tabs_button.on_menu_request = [this, &tabs_button] {
        if (on_tabs_menu_request)
            on_tabs_menu_request(tabs_button);
    };

    m_location_box = m_toolbar->add<GUI::TextBox>();
    m_location_box->set_placeholder("Search or enter address"sv);
    m_location_box->set_preferred_width(GUI::SpecialDimension::Grow);
    m_location_box->set_fixed_height(24);
    m_location_box->on_return_pressed = [this] {
        if (m_autocomplete_visible) {
            accept_autocomplete();
            return;
        }

        auto text = String::from_byte_string(m_location_box->text()).release_value();
        if (text.is_empty())
            return;
        navigate(text);
        m_web_view->set_focus(true);
    };
    m_location_box->on_change = [this] {
        if (m_location_box->text().is_empty()) {
            hide_autocomplete();
            return;
        }

        if (m_autocomplete_timer) {
            m_autocomplete_timer->stop();
            m_autocomplete_timer->start();
        }
    };
    m_location_box->on_up_pressed = [this] {
        move_autocomplete_selection(-1);
    };
    m_location_box->on_down_pressed = [this] {
        move_autocomplete_selection(1);
    };
    m_location_box->on_escape_pressed = [this] {
        hide_autocomplete();
    };
    m_location_box->on_focusin = [this] {
        if (!m_location_box->text().is_empty() && m_autocomplete_timer) {
            m_autocomplete_timer->stop();
            m_autocomplete_timer->start();
        }
    };
    m_location_box->on_focusout = [this] {
        if (m_autocomplete_hide_timer)
            m_autocomplete_hide_timer->start();
    };

    m_autocomplete_overlay = GUI::Widget::construct();
    m_autocomplete_overlay->set_fill_with_background_color(true);
    m_autocomplete_overlay->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 0 }, 0);
    m_autocomplete_overlay->set_visible(false);

    auto autocomplete_widget = m_autocomplete_overlay;

    Vector<GUI::JsonArrayModel::FieldSpec> autocomplete_fields;
    autocomplete_fields.append({ "title"sv, "Title"_string, Gfx::TextAlignment::CenterLeft });
    autocomplete_fields.append({ "display"sv, "Address"_string, Gfx::TextAlignment::CenterLeft });
    m_autocomplete_model = GUI::JsonArrayModel::create_from_source([this]() -> ErrorOr<JsonArray> {
        return m_autocomplete_rows;
    },
        move(autocomplete_fields));

    m_autocomplete_table = autocomplete_widget->add<GUI::TableView>();
    m_autocomplete_table->set_model(m_autocomplete_model);
    m_autocomplete_table->set_frame_style(Gfx::FrameStyle::Plain);
    m_autocomplete_table->set_column_headers_visible(false);
    m_autocomplete_table->set_activates_on_selection(true);
    m_autocomplete_table->on_activation = [this](auto const&) {
        accept_autocomplete();
    };

    m_autocomplete_timer = Core::Timer::create_single_shot(120, [this] {
        request_autocomplete();
    });
    m_autocomplete_hide_timer = Core::Timer::create_single_shot(50, [this] {
        // A click on the suggestion list moves focus to the table; let that
        // click finish instead of closing the list underneath it.
        if (m_autocomplete_table && m_autocomplete_table->is_focused())
            return;
        hide_autocomplete();
    });

    auto& zoom_button = m_toolbar->add<MenuButton>("100%"_string);
    zoom_button.set_button_style(Gfx::ButtonStyle::Coolbar);
    zoom_button.set_fixed_height(22);
    zoom_button.set_fixed_width(52);
    zoom_button.on_menu_request = [this, &zoom_button] {
        if (m_zoom_menu)
            m_zoom_menu->popup(zoom_button.screen_relative_rect().bottom_left(), {}, zoom_button.rect());
    };
    m_zoom_button = zoom_button;

    m_zoom_menu = GUI::Menu::construct("Zoom"_string);
    m_zoom_menu->add_action(GUI::Action::create("Zoom &In"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "in"sv);
    }));
    m_zoom_menu->add_action(GUI::Action::create("Zoom &Out"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "out"sv);
    }));
    m_zoom_menu->add_action(GUI::Action::create("&Reset Zoom"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "reset"sv);
    }));

    m_find_bar = m_root->add<GUI::Widget>();
    m_find_bar->set_fixed_height(30);
    m_find_bar->set_preferred_width(GUI::SpecialDimension::Grow);
    m_find_bar->set_layout<GUI::HorizontalBoxLayout>(GUI::Margins { 4 }, 4);
    m_find_bar->set_visible(false);

    auto& find_label = m_find_bar->add<GUI::Label>("Find:"_string);
    find_label.set_fixed_width(40);
    m_find_box = m_find_bar->add<GUI::TextBox>();
    m_find_box->set_fixed_width(160);
    m_find_box->on_change = [this] {
        request_find();
    };
    m_find_box->on_return_pressed = [this] {
        if (is_attached())
            m_engine.find_next(m_tab_id);
    };
    m_find_box->on_escape_pressed = [this] {
        hide_find_bar();
    };

    m_match_case = m_find_bar->add<GUI::CheckBox>("Match case"_string);
    m_match_case->set_fixed_width(90);
    m_match_case->on_checked = [this](bool) {
        request_find();
    };

    auto& previous_button = m_find_bar->add<GUI::Button>("Previous"_string);
    previous_button.set_fixed_width(60);
    previous_button.set_fixed_height(22);
    previous_button.on_click = [this](auto) {
        if (is_attached())
            m_engine.find_previous(m_tab_id);
    };

    auto& next_button = m_find_bar->add<GUI::Button>("Next"_string);
    next_button.set_fixed_width(40);
    next_button.set_fixed_height(22);
    next_button.on_click = [this](auto) {
        if (is_attached())
            m_engine.find_next(m_tab_id);
    };

    m_find_status = m_find_bar->add<GUI::Label>(String {});
    m_find_status->set_fixed_width(50);
    m_find_status->set_text_alignment(Gfx::TextAlignment::CenterRight);

    auto& close_button = m_find_bar->add<GUI::Button>("Close"_string);
    close_button.set_fixed_width(45);
    close_button.set_fixed_height(22);
    close_button.on_click = [this](auto) {
        hide_find_bar();
    };


    m_web_view = m_root->add<WebContentView>();
    m_web_view->set_preferred_size(GUI::SpecialDimension::Grow, GUI::SpecialDimension::Grow);

    m_statusbar = m_root->add<GUI::Statusbar>();
    m_statusbar->set_fixed_height(22);
    m_statusbar->set_text(String {});

    m_web_view->on_resize = [this](Gfx::IntSize size) {
        hide_autocomplete();
        if (is_attached())
            m_engine.resize(m_tab_id, size);
    };
    m_web_view->on_mouse = [this](StringView type, Gfx::IntPoint position, GUI::MouseButton button, unsigned buttons, unsigned modifiers, int click_count) {
        if (!is_attached())
            return;
        m_engine.mouse_event(m_tab_id, type, position, StringView { button_name(button) }, buttons, modifiers, click_count);
    };
    m_web_view->on_wheel = [this](Gfx::IntPoint position, int delta_x, int delta_y, unsigned modifiers) {
        if (is_attached())
            m_engine.wheel_event(m_tab_id, position, delta_x, delta_y, modifiers);
    };
    m_web_view->on_key = [this](StringView type, StringView key_name, u32 code_point, unsigned modifiers) {
        if (is_attached())
            m_engine.key_event(m_tab_id, type, key_name, code_point, modifiers);
    };
    m_web_view->on_focus_change = [this](bool focused) {
        if (is_attached())
            m_engine.set_focus(m_tab_id, focused);
    };
    m_web_view->on_go_back = [this] { go_back(); };
    m_web_view->on_go_forward = [this] { go_forward(); };
    m_web_view->on_reload = [this] { reload(); };
    m_web_view->on_force_reload = [this] {
        if (on_hard_reload)
            on_hard_reload();
    };
    m_web_view->on_frame_ack = [this](u64 sequence) {
        if (is_attached())
            m_engine.frame_ack(m_tab_id, sequence);
    };

    // The suggestion list is an in-window overlay, not a separate window. It
    // cannot take keyboard focus from the location box or float above other
    // applications when the browser is minimized or covered. Removing it from
    // the root layout lets it float over the bookmark bar and page.
    m_root->add_child(*m_autocomplete_overlay);
    if (m_root->layout())
        m_root->layout()->remove_widget(*m_autocomplete_overlay);

    (void)window;
    set_zoom_level(1.0);
    update_site_button();
}

StringView BrowserTab::button_name(GUI::MouseButton button)
{
    switch (button) {
    case GUI::MouseButton::Primary:
        return "left"sv;
    case GUI::MouseButton::Secondary:
        return "right"sv;
    case GUI::MouseButton::Middle:
        return "middle"sv;
    case GUI::MouseButton::Backward:
        return "back"sv;
    case GUI::MouseButton::Forward:
        return "forward"sv;
    default:
        return "none"sv;
    }
}

Gfx::IntSize BrowserTab::content_size() const
{
    auto size = m_web_view->size();
    if (size.is_empty())
        return { 1024, 768 };
    return size;
}

void BrowserTab::create_engine_tab(u64 request_id, String const& url, bool private_tab)
{
    m_request_id = request_id;
    m_initial_url = url;
    m_private = private_tab;
    m_engine_tab_requested = true;
    m_engine.create_tab(content_size(), url, request_id, private_tab, m_window_id, m_session_tab_id, m_session_window_id);
}

void BrowserTab::attach_tab_id(u64 tab_id)
{
    m_tab_id = tab_id;
    m_engine_tab_requested = true;
    m_lazy = false;
    m_engine.activate_tab(tab_id, m_window_id);
    m_engine.resize(tab_id, content_size());
    m_engine.set_focus(tab_id, m_web_view->is_focused());
}

void BrowserTab::detach()
{
    m_tab_id = 0;
    m_engine_tab_requested = false;
}

void BrowserTab::navigate(String const& url)
{
    hide_autocomplete();
    if (is_attached())
        m_engine.navigate(m_tab_id, url, true);
}

void BrowserTab::reload()
{
    if (is_attached())
        m_engine.reload(m_tab_id);
}

void BrowserTab::stop()
{
    if (is_attached())
        m_engine.stop(m_tab_id);
}

void BrowserTab::go_back()
{
    if (is_attached())
        m_engine.go_back(m_tab_id);
}

void BrowserTab::go_forward()
{
    if (is_attached())
        m_engine.go_forward(m_tab_id);
}

void BrowserTab::focus_location()
{
    m_location_box->set_focus(true);
    m_location_box->select_all();
}

void BrowserTab::show_find_bar()
{
    m_find_bar->set_visible(true);
    if (auto* parent = m_find_bar->parent_widget())
        parent->invalidate_layout();
    m_root->do_layout();
    if (auto* window = m_root->window())
        window->update();
    m_find_box->set_focus(true);
    m_find_box->select_all();
}

void BrowserTab::hide_find_bar()
{
    if (auto* parent = m_find_bar->parent_widget())
        parent->invalidate_layout();
    m_root->do_layout();
    if (auto* window = m_root->window())
        window->update();
    m_find_bar->set_visible(false);
    if (is_attached())
        m_engine.find_in_page(m_tab_id, String {});
    m_web_view->set_focus(true);
}

void BrowserTab::add_bookmark_menu_items(GUI::Menu& menu, Vector<BookmarkEntry> const& items)
{
    for (auto const& item : items) {
        if (item.is_folder) {
            auto submenu = menu.add_submenu(item.title);
            add_bookmark_menu_items(*submenu, item.children);
        } else {
            menu.add_action(GUI::Action::create(item.title.bytes_as_string_view(), [this, url = item.url](auto&) {
                navigate(url);
            }));
        }
    }
}

static int bookmark_button_width(String const& title, bool is_folder)
{
    auto width = static_cast<int>(title.bytes_as_string_view().length()) * 7 + 24;
    if (is_folder)
        width += 12;
    return min(width, 240);
}

void BrowserTab::set_bookmarks(Vector<BookmarkEntry> const& bookmarks)
{
    Vector<NonnullRefPtr<GUI::Widget>> children;
    m_bookmarks_bar->for_each_child_widget([&](GUI::Widget& child) {
        children.append(child);
        return IterationDecision::Continue;
    });
    for (auto& child : children)
        m_bookmarks_bar->remove_child(*child);

    if (!bookmarks.is_empty()) {
        // A stable overflow entry at the leading edge keeps every bookmark
        // reachable when the bar is narrower than its contents.
        auto& overflow = m_bookmarks_bar->add<MenuButton>("»"_string);
        overflow.set_button_style(Gfx::ButtonStyle::Coolbar);
        overflow.set_fixed_height(22);
        overflow.set_fixed_width(28);
        auto overflow_menu = GUI::Menu::construct("Bookmarks"_string);
        add_bookmark_menu_items(*overflow_menu, bookmarks);
        overflow.on_menu_request = [&overflow, menu = move(overflow_menu)] {
            menu->popup(overflow.screen_relative_rect().bottom_left(), {}, overflow.rect());
        };
    }

    for (auto const& bookmark : bookmarks) {
        auto title = bookmark.title;
        if (bookmark.is_folder)
            title = String::formatted("{} ▸", title).release_value();

        auto& button = m_bookmarks_bar->add<MenuButton>(title);
        button.set_button_style(Gfx::ButtonStyle::Coolbar);
        button.set_fixed_height(22);
        button.set_fixed_width(bookmark_button_width(title, bookmark.is_folder));

        if (bookmark.is_folder) {
            auto menu = GUI::Menu::construct(title);
            add_bookmark_menu_items(*menu, bookmark.children);
            button.on_menu_request = [&button, menu = move(menu)] {
                menu->popup(button.screen_relative_rect().bottom_left(), {}, button.rect());
            };
        } else {
            button.set_tooltip(bookmark.url);
            button.on_click = [this, url = bookmark.url](auto) {
                navigate(url);
            };
        }
    }

    m_has_bookmarks = !bookmarks.is_empty();
    m_bookmarks_bar->set_visible(m_bookmarks_bar_visible && m_has_bookmarks);
}

void BrowserTab::set_bookmarks_bar_visible(bool visible)
{
    m_bookmarks_bar_visible = visible;
    m_bookmarks_bar->set_visible(visible && m_has_bookmarks);
}

void BrowserTab::set_bookmarked(bool bookmarked)
{
    if (m_bookmarked == bookmarked)
        return;
    m_bookmarked = bookmarked;
    if (on_bookmark_change)
        on_bookmark_change(bookmarked);
}

void BrowserTab::set_loading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    if (loading)
        m_web_view->set_crashed(false);
    m_statusbar->set_text(loading ? "Loading..."_string : String {});
    if (on_loading_change)
        on_loading_change(loading);
}

void BrowserTab::set_history(bool can_go_back, bool can_go_forward)
{
    m_can_go_back = can_go_back;
    m_can_go_forward = can_go_forward;
}

void BrowserTab::set_url(String const& url)
{
    hide_autocomplete();

    if (m_location_box->text() != url.to_byte_string()) {
        m_location_box->set_text(url.bytes_as_string_view());
        if (on_url_change)
            on_url_change(url);
    }
    update_site_button();
}

void BrowserTab::set_zoom_level(double zoom_level)
{
    if (zoom_level <= 0)
        zoom_level = 1.0;
    m_zoom_level = zoom_level;

    if (!m_zoom_button)
        return;

    auto percent = static_cast<int>(zoom_level * 100.0 + 0.5);
    auto text = String::formatted("{}%", percent).release_value();
    m_zoom_button->set_text(text);
}

void BrowserTab::update_site_button()
{
    if (!m_site_button)
        return;

    auto url = current_url();
    auto identity = url.is_empty()
        ? SiteIdentity { "No page loaded"_string, {} }
        : site_identity_for_url(url);

    auto label = "Site"_string;
    if (identity.security == "Secure connection"sv)
        label = "Secure"_string;
    else if (identity.security == "Not secure"sv)
        label = "Not secure"_string;
    else if (identity.security == "Local file"sv)
        label = "Local"_string;
    else if (identity.security == "Browser page"sv)
        label = "Page"_string;
    m_site_button->set_text(label);

    auto menu = GUI::Menu::construct("Site"_string);
    auto add_information = [&](String const& text) {
        auto action = GUI::Action::create(text.bytes_as_string_view(), [](auto&) {});
        action->set_enabled(false);
        menu->add_action(action);
    };

    add_information(identity.security);
    if (!identity.host.is_empty())
        add_information(identity.host);

    menu->add_separator();
    menu->add_action(GUI::Action::create("Zoom &In"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "in"sv);
    }));
    menu->add_action(GUI::Action::create("Zoom &Out"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "out"sv);
    }));
    menu->add_action(GUI::Action::create("&Reset Zoom"sv, [this](auto&) {
        if (is_attached())
            m_engine.set_zoom(m_tab_id, "reset"sv);
    }));
    if (m_permission_summary_provider) {
        auto summary = m_permission_summary_provider(identity.host);
        if (!summary.is_empty()) {
            menu->add_separator();
            add_information(String::formatted("Permissions: {}", summary).release_value());
            menu->add_action(GUI::Action::create("Forget Site Permissions"sv, [this, host = identity.host](auto&) {
                if (m_forget_permissions_handler)
                    m_forget_permissions_handler(host);
                // Rebuilding the menu while it is being activated destroys the popup out of order.
                Core::deferred_invoke([this] {
                    update_site_button();
                });
            }));
        }
    }

    menu->add_separator();
    menu->add_action(GUI::Action::create("Site &Settings"sv, [this](auto&) {
        navigate("about:settings"_string);
    }));

    m_site_menu = menu;
}

void BrowserTab::show_site_info_dialog()
{
    auto url = current_url();
    auto identity = url.is_empty()
        ? SiteIdentity { "No page loaded"_string, {} }
        : site_identity_for_url(url);
    auto percent = static_cast<int>(m_zoom_level * 100.0 + 0.5);
    auto subject = identity.host.is_empty() ? url : identity.host;
    auto text = String::formatted("{}: {}\nZoom: {}%", identity.security, subject, percent).release_value();
    if (m_permission_summary_provider) {
        auto summary = m_permission_summary_provider(identity.host);
        if (!summary.is_empty())
            text = String::formatted("{}\nPermissions: {}", text, summary).release_value();
    }
    GUI::MessageBox::show(m_root->window(), text, "Site Information"sv, GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK);
}

void BrowserTab::set_autocomplete_results(u64 request_id, JsonArray const& items, bool)
{
    if (request_id != m_autocomplete_request_id)
        return;

    // A result that arrives after the location box lost focus must not reopen
    // the list over the page.
    if (!m_location_box->is_focused()) {
        hide_autocomplete();
        return;
    }

    if (items.is_empty()) {
        hide_autocomplete();
        return;
    }

    m_autocomplete_rows = items;
    m_autocomplete_texts.clear();
    m_autocomplete_texts.ensure_capacity(m_autocomplete_rows.size());

    for (auto const& value : m_autocomplete_rows.values()) {
        if (!value.is_object())
            continue;
        auto text = value.as_object().get_byte_string("text"sv).value_or({});
        m_autocomplete_texts.append(String::from_byte_string(text).release_value());
    }

    if (!m_autocomplete_model || !m_autocomplete_table || !m_autocomplete_overlay)
        return;

    m_autocomplete_model->update();
    if (m_autocomplete_model->row_count() == 0) {
        hide_autocomplete();
        return;
    }

    m_autocomplete_table->set_cursor(m_autocomplete_model->index(0), GUI::AbstractView::SelectionUpdate::Set);

    auto location_rect = m_location_box->screen_relative_rect();
    auto root_rect = m_root->screen_relative_rect();
    auto x = max(0, location_rect.x() - root_rect.x());
    auto y = max(0, location_rect.bottom() - root_rect.y());
    auto available_width = max(1, m_root->width() - x);
    auto available_height = max(1, m_root->height() - y - (m_statusbar ? m_statusbar->height() : 0));
    auto width = min(max(location_rect.width(), 420), available_width);
    auto height = min(300, m_autocomplete_model->row_count() * 24 + 4);
    height = min(height, available_height);
    m_autocomplete_overlay->set_relative_rect(Gfx::IntRect { x, y, width, height });
    m_autocomplete_table->set_column_width(0, width / 3);
    m_autocomplete_table->set_column_width(1, width - width / 3);
    m_autocomplete_overlay->set_visible(true);

    m_autocomplete_visible = true;
    if (m_autocomplete_hide_timer)
        m_autocomplete_hide_timer->stop();
}

void BrowserTab::request_autocomplete()
{
    if (!is_attached())
        return;

    auto query = String::from_byte_string(m_location_box->text()).release_value();
    if (query.is_empty()) {
        hide_autocomplete();
        return;
    }

    auto request_id = ++m_autocomplete_request_id;
    m_engine.autocomplete(m_tab_id, request_id, query);
}

void BrowserTab::hide_autocomplete()
{
    if (m_autocomplete_hide_timer)
        m_autocomplete_hide_timer->stop();
    if (m_autocomplete_overlay)
        m_autocomplete_overlay->set_visible(false);

    m_autocomplete_visible = false;
    // Invalidate any request still in flight so a late result cannot reopen
    // the list after navigation or focus loss.
    ++m_autocomplete_request_id;
    m_autocomplete_rows.clear();
    m_autocomplete_texts.clear();
    if (m_autocomplete_model)
        m_autocomplete_model->update();
}

void BrowserTab::accept_autocomplete()
{
    auto selection = m_autocomplete_table ? m_autocomplete_table->selection().first() : GUI::ModelIndex {};
    if (!selection.is_valid()) {
        hide_autocomplete();
        return;
    }

    auto row = selection.row();
    if (row < 0 || row >= static_cast<int>(m_autocomplete_texts.size())) {
        hide_autocomplete();
        return;
    }

    auto text = m_autocomplete_texts[row];
    hide_autocomplete();
    if (text.is_empty())
        return;

    m_location_box->set_text(text.bytes_as_string_view());
    navigate(text);
    m_web_view->set_focus(true);
}

void BrowserTab::move_autocomplete_selection(int delta)
{
    if (!m_autocomplete_visible || !m_autocomplete_table || delta == 0)
        return;

    auto movement = delta < 0
        ? GUI::AbstractView::CursorMovement::Up
        : GUI::AbstractView::CursorMovement::Down;
    m_autocomplete_table->move_cursor(movement, GUI::AbstractView::SelectionUpdate::Set);
}

void BrowserTab::set_title(String const& title)
{
    m_title = title.is_empty() ? "Untitled"_string : title;
    if (on_title_change)
        on_title_change(m_title);
}

void BrowserTab::set_favicon(StringView path)
{
    if (path.is_empty()) {
        m_favicon.clear();
        if (on_favicon_change)
            on_favicon_change(nullptr);
        return;
    }

    auto bitmap = Gfx::Bitmap::load_from_file(path, 1, Gfx::IntSize { 16, 16 });
    if (bitmap.is_error())
        return;
    m_favicon = bitmap.value();
    if (on_favicon_change)
        on_favicon_change(bitmap.value().ptr());
}

void BrowserTab::set_cursor(StringView name)
{
    m_web_view->set_page_cursor(name);
}

void BrowserTab::set_status(StringView text)
{
    m_statusbar->set_override_text(text.is_empty() ? Optional<String> {} : String::from_utf8(text).release_value());
}


String BrowserTab::current_url() const
{
    return String::from_byte_string(m_location_box->text()).release_value();
}

void BrowserTab::set_crashed(bool crashed)
{
    m_web_view->set_crashed(crashed);
    if (crashed)
        set_status("The page crashed. Click the page to reload."sv);
}

void BrowserTab::set_unresponsive(bool unresponsive)
{
    m_web_view->set_unresponsive(unresponsive);
    set_status(unresponsive ? "The page is not responding. Click the banner to reload."sv : ""sv);
}

void BrowserTab::set_find_result(u64 current, Optional<u64> total)
{
    if (!m_find_status)
        return;
    if (!total.has_value() || *total == 0) {
        m_find_status->set_text("No matches"_string);
        return;
    }
    m_find_status->set_text(String::formatted("{}/{}", current, *total).release_value());
}

void BrowserTab::add_context_menu_items(GUI::Menu& menu, JsonArray const& items)
{
    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        if (object.get_bool("separator"sv).value_or(false)) {
            menu.add_separator();
            continue;
        }

        auto title = String::from_byte_string(object.get_byte_string("title"sv).value_or({})).release_value();
        if (auto submenu_items = object.get_array("items"sv); submenu_items.has_value()) {
            auto submenu = menu.add_submenu(title);
            add_context_menu_items(*submenu, *submenu_items);
            continue;
        }

        auto text = object.get_byte_string("text"sv).value_or({});
        auto action_index = object.get_u64("action"sv);
        if (!action_index.has_value())
            continue;

        auto checkable = object.get_bool("checkable"sv).value_or(false);
        auto enabled = object.get_bool("enabled"sv).value_or(true);

        NonnullRefPtr<GUI::Action> action = checkable
            ? GUI::Action::create_checkable(text, [this, index = *action_index](auto&) {
                  if (is_attached())
                      m_engine.context_menu_action(m_tab_id, index);
              })
            : GUI::Action::create(text, [this, index = *action_index](auto&) {
                  if (is_attached())
                      m_engine.context_menu_action(m_tab_id, index);
              });
        if (checkable)
            action->set_checked(object.get_bool("checked"sv).value_or(false));

        action->set_enabled(enabled);
        menu.add_action(action);
    }
}

void BrowserTab::show_context_menu(JsonArray const& items, Gfx::IntPoint view_position)
{
    auto menu = GUI::Menu::construct("Context Menu"_string);
    add_context_menu_items(*menu, items);

    auto root_origin = m_root->screen_relative_rect().location();
    auto view_origin = m_web_view->screen_relative_rect().location();
    auto popup_position = view_origin.translated(-root_origin.x(), -root_origin.y()).translated(view_position.x(), view_position.y());

    m_context_menu = menu;
    m_context_menu->popup(popup_position);
}

void BrowserTab::show_permission_prompt(StringView name, StringView origin, u64 request_id)
{
    auto message = String::formatted("Allow {} for {}?", name, origin).release_value();
    auto result = GUI::MessageBox::show(m_root->window(), message, "Permission Request"sv,
        GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
    auto granted = result == GUI::Dialog::ExecResult::Yes;
    if (on_permission_decision)
        on_permission_decision(String::from_utf8(origin).release_value(), String::from_utf8(name).release_value(), granted);
    if (is_attached())
        m_engine.permission_response(m_tab_id, request_id, granted);
}

void BrowserTab::show_file_picker(bool, JsonArray const& filters)
{
    // LibGUI's file picker exposes one path at a time, so a multiple-file
    // request still opens a single selection. The engine accepts a list, so the
    // dialog can become multi-select without changing the protocol.
    Optional<Vector<GUI::FileTypeFilter>> allowed_filters;
    Vector<GUI::FileTypeFilter> parsed_filters;

    for (auto const& value : filters.values()) {
        if (!value.is_string())
            continue;
        auto filter = String::from_byte_string(value.as_string()).release_value();
        auto view = filter.bytes_as_string_view();
        if (view == "Image files"sv)
            parsed_filters.append(GUI::FileTypeFilter::image_files());
        else if (view == "Audio files"sv)
            parsed_filters.append(GUI::FileTypeFilter::audio_files());
        else if (view == "Video files"sv || view.starts_with("video/"sv))
            parsed_filters.append(GUI::FileTypeFilter::video_files());
        else if (view.starts_with("audio/"sv))
            parsed_filters.append(GUI::FileTypeFilter::audio_files());
        else if (view.starts_with("image/"sv))
            parsed_filters.append(GUI::FileTypeFilter::image_files());
        else if (view.starts_with("*."sv))
            parsed_filters.append({ "Files"sv, Vector<ByteString> { filter.bytes_as_string_view().substring_view(2) } });
    }
    if (!parsed_filters.is_empty())
        allowed_filters = move(parsed_filters);

    auto path = GUI::FilePicker::get_open_filepath(m_root->window(), "Select File"sv, Core::StandardPaths::home_directory(), false, GUI::Dialog::ScreenPosition::CenterWithinParent, allowed_filters);
    if (!is_attached())
        return;

    if (path.has_value())
        m_engine.file_picker_response(m_tab_id, String::from_byte_string(path.release_value()).release_value());
    else
        m_engine.file_picker_response(m_tab_id, String {});
}

void BrowserTab::request_find()
{
    if (!is_attached())
        return;
    if (!m_find_box || m_find_box->text().is_empty()) {
        m_engine.find_in_page(m_tab_id, String {});
        return;
    }
    m_engine.find_in_page(m_tab_id, String::from_byte_string(m_find_box->text()).release_value(), m_match_case && m_match_case->is_checked());
}

void BrowserTab::set_audio_state(bool playing, bool muted)
{
    m_audio_playing = playing;
    m_muted = muted;
}

void BrowserTab::add_select_dropdown_items(GUI::Menu& menu, JsonArray const& items, bool in_group)
{
    for (auto const& value : items.values()) {
        if (!value.is_object())
            continue;

        auto const& object = value.as_object();
        if (object.get_bool("separator"sv).value_or(false)) {
            menu.add_separator();
            continue;
        }

        if (auto group_items = object.get_array("items"sv); group_items.has_value()) {
            auto group_label = String::from_byte_string(object.get_byte_string("group"sv).value_or({})).release_value();
            auto group_action = GUI::Action::create(group_label.to_byte_string(), [](auto&) {});
            group_action->set_enabled(false);
            menu.add_action(group_action);
            add_select_dropdown_items(menu, *group_items, true);
            continue;
        }

        auto label = String::from_byte_string(object.get_byte_string("label"sv).value_or({})).release_value();
        if (in_group)
            label = String::formatted("    {}", label).release_value();
        auto id = object.get_u64("id"sv);
        if (!id.has_value())
            continue;

        auto action = GUI::Action::create_checkable(label.to_byte_string(), [this, item_id = static_cast<u32>(*id)](auto&) {
            m_selected_select_item = item_id;
        });
        action->set_checked(object.get_bool("selected"sv).value_or(false));
        action->set_enabled(!object.get_bool("disabled"sv).value_or(false));
        menu.add_action(action);
    }
}

void BrowserTab::show_select_dropdown(Gfx::IntPoint position, i32 minimum_width, JsonArray const& items)
{
    auto menu = GUI::Menu::construct("Select Dropdown"_string);
    menu->set_minimum_width(minimum_width);
    add_select_dropdown_items(*menu, items);

    m_selected_select_item.clear();
    m_select_dropdown_visible = false;
    menu->on_visibility_change = [this](bool visible) {
        if (visible) {
            m_select_dropdown_visible = true;
            return;
        }
        if (!m_select_dropdown_visible)
            return;

        m_select_dropdown_visible = false;
        auto selected = m_selected_select_item;
        m_selected_select_item.clear();
        if (is_attached())
            m_engine.select_dropdown_closed(m_tab_id, selected);
    };

    m_select_dropdown = menu;
    auto root_origin = m_root->screen_relative_rect().location();
    auto view_origin = m_web_view->screen_relative_rect().location();
    auto popup_position = view_origin.translated(-root_origin.x(), -root_origin.y()).translated(position.x(), position.y());
    m_select_dropdown->popup(popup_position);
}

void BrowserTab::show_dialog(StringView kind, StringView message, StringView default_text)
{
    auto& window = *m_root->window();
    auto text = String::from_utf8(message).release_value();

    if (kind == "alert"sv) {
        GUI::MessageBox::show(&window, text, "Alert"sv, GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK);
        m_engine.dialog_response(m_tab_id, "alert"sv, true);
        return;
    }

    if (kind == "confirm"sv) {
        auto result = GUI::MessageBox::show(&window, text, "Confirm"sv, GUI::MessageBox::Type::Question, GUI::MessageBox::InputType::YesNo);
        m_engine.dialog_response(m_tab_id, "confirm"sv, result == GUI::Dialog::ExecResult::Yes);
        return;
    }

    if (kind == "prompt"sv) {
        auto value = String::from_utf8(default_text).release_value();
        auto result = GUI::InputBox::show(&window, value, text, "Prompt"sv, GUI::InputType::Text);
        m_engine.dialog_response(m_tab_id, "prompt"sv, result == GUI::Dialog::ExecResult::OK, value);
    }
}

}
