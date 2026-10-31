#!/usr/bin/env bash
# Run the LibGUI Ladybird from a stable live snapshot.
#
# The snapshot is made with scripts/install-live.sh. Running the build tree directly
# is unsafe while it is being rebuilt: an engine host that is already running maps its
# old libraries, but a helper it restarts (RequestServer after a resolver change, say)
# would start from the new tree and can fail with an ABI mismatch.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
LIVE="${LADYBIRD_LIVE_DIR:-$ROOT/build/live}"

if [[ -z "${LADYBIRD_BUILD:-}" && -x "$LIVE/bin/Ladybird" && -x "$LIVE/libexec/LadybirdEngine" ]]; then
    LADYBIRD_BUILD="$LIVE"
    LADYBIRD_CHROME="$LIVE/bin/Ladybird"
    # The snapshot's binaries carry $ORIGIN-relative RUNPATHs (the browser uses libgui
    # first, the engine and helpers use lib), so the environment cannot redirect them
    # at the build tree.
    unset LD_LIBRARY_PATH
else
    LADYBIRD_BUILD="${LADYBIRD_BUILD:-$ROOT/Build/release}"
    LADYBIRD_CHROME="${LADYBIRD_CHROME:-$ROOT/build/chrome/Ladybird}"
    unset LD_LIBRARY_PATH
    if [[ -x "$LIVE/bin/Ladybird" ]]; then
        echo "note: LADYBIRD_BUILD is set; running the build tree, not the live snapshot." >&2
    else
        echo "note: no live snapshot at $LIVE; run scripts/install-live.sh for a stable session." >&2
    fi
fi

export LADYBIRD_SOURCE_DIR="${LADYBIRD_SOURCE_DIR:-$ROOT}"
export LADYBIRD_ENGINE_HOST="${LADYBIRD_ENGINE_HOST:-$LADYBIRD_BUILD/libexec/LadybirdEngine}"
export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"

exec "$LADYBIRD_CHROME" "$@"
