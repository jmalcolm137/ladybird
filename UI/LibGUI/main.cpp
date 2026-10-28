/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BrowserWindow.h"

#include <LibGUI/Application.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    Vector<String> urls;
    bool private_window = false;
    for (size_t i = 1; i < arguments.strings.size(); ++i) {
        auto argument = arguments.strings[i];
        if (argument == "--private"sv || argument == "-p"sv) {
            private_window = true;
            continue;
        }
        if (argument.starts_with('-'))
            continue;
        urls.append(String::from_utf8(argument).release_value());
    }

    auto window = LadybirdSDE::BrowserWindow::create(move(urls), private_window);
    if (window.is_error()) {
        warnln("Ladybird: {}", window.error());
        return 1;
    }

    window.value()->show();
    return app->exec();
}
