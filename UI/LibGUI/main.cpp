/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserSession.h"

#include <LibCore/EventLoop.h>
#include <LibGUI/Application.h>
#include <LibMain/Main.h>
#include <signal.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    // A session logout or SIGTERM ends like File -> Quit: save the session before leaving instead of disappearing
    // without writing geometry and the final task state.
    for (auto signal_number : { SIGINT, SIGTERM }) {
        Core::EventLoop::register_signal(signal_number, [](int) {
            GUI::Application::the()->quit(0);
        });
    }

    Vector<String> urls;
    bool private_window = false;
    Optional<u16> devtools_port;
    for (size_t i = 1; i < arguments.strings.size(); ++i) {
        auto argument = arguments.strings[i];
        if (argument == "--private"sv || argument == "-p"sv) {
            private_window = true;
            continue;
        }
        if (argument == "--devtools"sv) {
            devtools_port = 0;
            continue;
        }
        if (argument.starts_with("--devtools="sv)) {
            auto value = argument.substring_view("--devtools="sv.length());
            if (auto port = value.to_number<u16>(); port.has_value())
                devtools_port = *port;
            continue;
        }
        if (argument.starts_with('-'))
            continue;
        urls.append(String::from_utf8(argument).release_value());
    }

    auto session = LadybirdSDE::BrowserSession::create(move(urls), private_window, devtools_port);
    if (session.is_error()) {
        warnln("Ladybird: {}", session.error());
        return 1;
    }

    session.value()->show();
    return app->exec();
}
