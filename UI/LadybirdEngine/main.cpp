/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EngineApplication.h"
#include "EngineServer.h"

#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <signal.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto app = TRY(LadybirdSDE::EngineApplication::create(arguments));

    if (app->engine_socket_path().is_empty()) {
        warnln("LadybirdEngine: --engine-socket is required");
        return 1;
    }
    if (app->frame_directory().is_empty()) {
        warnln("LadybirdEngine: --frame-directory is required");
        return 1;
    }

    auto socket = TRY(Core::LocalSocket::connect(app->engine_socket_path()));

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root.view(), "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto server = make<LadybirdSDE::EngineServer>(move(socket), app->frame_directory(), move(theme));
    TRY(server->start());

    Core::EventLoop::register_signal(SIGINT, [](int) {
        Core::EventLoop::current().quit(0);
    });
    Core::EventLoop::register_signal(SIGTERM, [](int) {
        Core::EventLoop::current().quit(0);
    });

    return Core::EventLoop::current().exec();
}
