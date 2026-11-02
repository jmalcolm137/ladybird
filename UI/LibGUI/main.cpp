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

    LadybirdSDE::BrowserSession* session_pointer = nullptr;

    // A session logout or SIGTERM ends like File -> Quit: save the session before leaving instead of disappearing
    // without writing geometry and the final task state. Record the signal so the session log says why it exited.
    // The handler only runs once the event loop does, by which point the session exists.
    for (auto signal_number : { SIGINT, SIGTERM }) {
        Core::EventLoop::register_signal(signal_number, [&session_pointer](int signal_number) {
            auto name = signal_number == SIGTERM ? "SIGTERM"sv : signal_number == SIGINT ? "SIGINT"sv : "signal"sv;
            if (session_pointer)
                session_pointer->note_exit_reason(ByteString::formatted("received {} ({})", name, signal_number));
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

    auto created_session = LadybirdSDE::BrowserSession::create(move(urls), private_window, devtools_port);
    if (created_session.is_error()) {
        warnln("Ladybird: {}", created_session.error());
        return 1;
    }

    auto* session = created_session.value().ptr();
    session_pointer = session;

    session->show();
    auto exit_status = app->exec();
    session->note_exit_reason(ByteString::formatted("event loop returned {}", exit_status));
    return exit_status;
}
