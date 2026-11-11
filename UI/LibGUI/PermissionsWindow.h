/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGUI/Forward.h>

namespace LadybirdSDE {

class BrowserSession;

// Global view of the decisions the site-info popover stores one origin at a
// time. The session owns the policy; this window only lists it and asks the
// session to forget an origin or everything.
class PermissionsWindow : public RefCounted<PermissionsWindow> {
public:
    static NonnullRefPtr<PermissionsWindow> create(BrowserSession&);
    ~PermissionsWindow();

    void show();
    void refresh();

private:
    explicit PermissionsWindow(BrowserSession&);

    JsonArray rows();
    int selected_row() const;
    void revoke_selected();

    BrowserSession& m_session;
    RefPtr<GUI::Window> m_window;
    RefPtr<GUI::TableView> m_table;
    RefPtr<GUI::JsonArrayModel> m_model;
    RefPtr<GUI::Label> m_summary;
    Vector<String> m_origins;
};

}
