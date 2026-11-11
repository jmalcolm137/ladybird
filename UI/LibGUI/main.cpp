/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserSession.h"

#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibIPC/Connection.h>
#include <LibMain/Main.h>
#include <WaylandClient.h>
#include <signal.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    // Closing the window must be handled by BrowserSession: it closes the
    // engine tabs, saves the session, and then quits. LibGUI's default is to
    // quit as soon as the last Window is hidden, which happens inside
    // Window::close() *before* Window::on_close() runs, so the session would
    // otherwise look like an unexplained "event loop returned 0" exit.
    app->set_quit_when_last_window_deleted(false);

    LadybirdSDE::BrowserSession* session_pointer = nullptr;

    // If a modal dialog is open, the nested event loop keeps running after
    // Application::quit(), so a session logout would hang. Give the normal
    // save-and-quit path a grace period, then leave regardless.
    static RefPtr<Core::Timer> shutdown_watchdog;

    // A session logout or SIGTERM ends like File -> Quit: save the session before leaving instead of disappearing
    // without writing geometry and the final task state. Record the signal so the session log says why it exited.
    // The handler only runs once the event loop does, by which point the session exists.
    for (auto signal_number : { SIGINT, SIGTERM }) {
        Core::EventLoop::register_signal(signal_number, [&session_pointer](int signal_number) {
            auto name = signal_number == SIGTERM ? "SIGTERM"sv : signal_number == SIGINT ? "SIGINT"sv : "signal"sv;
            if (session_pointer) {
                session_pointer->note_exit_reason(ByteString::formatted("received {} ({})", name, signal_number));
                session_pointer->quit();
            } else {
                GUI::Application::the()->quit(0);
            }
            if (!shutdown_watchdog)
                shutdown_watchdog = Core::Timer::create_single_shot(3000, [] { _exit(0); });
            shutdown_watchdog->start();
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

    // The compositor and window-manager connections are not optional: without
    // them there is no window to paint into. Their default failure path calls
    // exit(0) without running any destructor, which looks exactly like a crash
    // (no exit line, no session save). Record why the session ended and save it
    // instead. Both hooks run from the event loop, where the session is valid.
    IPC::ConnectionBase::connection_lost_handler() = [&session_pointer](StringView name) {
        if (!session_pointer) {
            GUI::Application::the()->quit(0);
            return;
        }
        session_pointer->note_exit_reason(ByteString::formatted("lost the connection to {} (the window manager is gone)", name));
        session_pointer->quit();
    };
    LibWM::WaylandClient::the().set_connection_lost_callback([&session_pointer](ByteString const& reason) {
        if (!session_pointer) {
            GUI::Application::the()->quit(0);
            return;
        }
        session_pointer->note_exit_reason(ByteString::formatted("lost the Wayland connection: {}", reason));
        session_pointer->quit();
    });

    session->show();
    auto exit_status = app->exec();

    // The hooks live in process-wide singletons and outlive this stack frame;
    // clear them before `session_pointer` (and the session) go away.
    IPC::ConnectionBase::connection_lost_handler() = {};
    LibWM::WaylandClient::the().set_connection_lost_callback({});

    session->note_exit_reason(ByteString::formatted("event loop returned {}", exit_status));
    return exit_status;
}
