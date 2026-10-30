/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "EngineClient.h"

#include <AK/JsonArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGUI/Forward.h>
#include <LibGfx/Forward.h>

namespace LadybirdSDE {

struct DownloadEntry {
    u64 id { 0 };
    String url;
    String destination;
    String status;
    u64 downloaded { 0 };
    Optional<u64> total;
    Optional<double> progress;
    bool can_resume { false };
    bool is_private { false };
    String error;
};

// The native downloads list. The engine owns the transfers; this window only
// displays their state and sends the user's action back over the control socket.
class DownloadsWindow : public RefCounted<DownloadsWindow> {
public:
    static NonnullRefPtr<DownloadsWindow> create(EngineClient&);
    ~DownloadsWindow();

    void set_downloads(Vector<DownloadEntry> const&);
    void set_private_filter(bool private_only);
    void show();

private:
    explicit DownloadsWindow(EngineClient&);

    void refresh();
    JsonArray rows() const;
    DownloadEntry const* selected_download() const;
    Vector<DownloadEntry const*> visible_downloads() const;
    void perform_action_on_selection(StringView action);
    void pause_or_resume_selection();

    EngineClient& m_engine;
    RefPtr<GUI::Window> m_window;
    RefPtr<GUI::TableView> m_table;
    RefPtr<GUI::JsonArrayModel> m_model;
    RefPtr<GUI::Label> m_summary;
    Vector<DownloadEntry> m_downloads;
    bool m_private_only { false };
};

}
