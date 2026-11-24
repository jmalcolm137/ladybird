/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibCore/Environment.h>

namespace LadybirdSDE {

// Escapes '&' to '&&' so untrusted page titles, bookmark names, and <select>
// option labels cannot inject unintended menu accelerator mnemonics in LibGUI.
inline String escape_menu_ampersands(StringView text)
{
    if (!text.contains('&'))
        return String::from_utf8_with_replacement_character(text);
    StringBuilder builder;
    for (char ch : text) {
        if (ch == '&')
            builder.append("&&"sv);
        else
            builder.append(ch);
    }
    return MUST(builder.to_string());
}

// Returns the sanitized LADYBIRD_SDE_PROFILE name, rejecting path separators
// or traversal components and falling back to "libgui".
inline ByteString sanitized_profile_name()
{
    auto profile = Core::Environment::get("LADYBIRD_SDE_PROFILE"sv);
    if (!profile.has_value() || profile->is_empty())
        return "libgui";
    auto view = *profile;
    if (view == "."sv || view == ".."sv || view.contains('/') || view.contains('\0'))
        return "libgui";
    return view.to_byte_string();
}

}
