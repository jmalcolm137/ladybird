/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

// The chrome (LibGUI) and the engine host (Ladybird) are separate processes with
// separate copies of AK. This header is the only code they share, so it must
// stay free of either library's types.

namespace LadybirdSDE {

// Bumped whenever a command or event changes shape incompatibly. The chrome
// refuses to talk to an engine host with a different version rather than fail
// mysteriously at the first unknown message.
constexpr uint32_t protocol_version = 9;

// Frame pixels live in a POSIX shared-memory ring owned by the engine host and
// mapped read-only by the chrome. The ring has a small number of fixed-size
// slots; a "frame" event names the slot and the live width, height and stride.

}
