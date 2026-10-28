#!/usr/bin/env bash
# Run the LibGUI Ladybird from the build trees, without installing it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LADYBIRD_BUILD="${LADYBIRD_BUILD:-$ROOT/Build/release}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

unset LD_LIBRARY_PATH
export LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR:-$ROOT}"
export LADYBIRD_ENGINE_HOST="${LADYBIRD_ENGINE_HOST:-$LADYBIRD_BUILD/libexec/LadybirdEngine}"
export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"

exec "$ROOT/build/chrome/Ladybird" "$@"
