#!/usr/bin/env bash
# Install the LibGUI Ladybird into an SDE prefix. The engine host is a normal
# Ladybird UI process, so its helper processes, libraries and resources move with
# it; the LibGUI frontend is a normal SDE application.
#
#   scripts/install-sde.sh                 # /usr/lib/sde-next (the side-by-side SDE)
#   SDE_PREFIX=/usr/lib/sde scripts/...    # a packaged SDE prefix
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LADYBIRD_BUILD="${LADYBIRD_BUILD:-$ROOT/Build/release}"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$ROOT/../libgui-wayland}"
LAGOM="${LAGOM:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
PREFIX="${SDE_PREFIX:-/usr/lib/sde-next}"
SUDO="${SUDO:-sudo}"

die() { echo "install-sde: $*" >&2; exit 1; }

[[ -x "$LADYBIRD_BUILD/libexec/LadybirdEngine" ]] || die "build the engine host first (scripts/build-engine.sh)"
[[ -x "$ROOT/build/chrome/Ladybird" ]] || die "build the frontend first (scripts/build-chrome.sh)"

install_file() {
    local source="$1" destination="$2" mode="${3:-0755}"
    local temporary="${destination}.new.$$"
    "$SUDO" install -m "$mode" "$source" "$temporary"
    "$SUDO" mv -f "$temporary" "$destination"
    echo "  $destination"
}

echo "==> installing Ladybird into $PREFIX"
"$SUDO" install -d "$PREFIX/bin" "$PREFIX/libexec" "$PREFIX/lib/ladybird" \
    "$PREFIX/share/Lagom" "$PREFIX/share/applications"

echo "==> frontend"
install_file "$ROOT/build/chrome/Ladybird" "$PREFIX/bin/Ladybird.real"
if command -v patchelf >/dev/null; then
    "$SUDO" patchelf --set-rpath '$ORIGIN/../lib' "$PREFIX/bin/Ladybird.real"
fi

# The wrapper gives the engine and its helpers the Ladybird libraries without
# requiring a system-wide linker path. The engine is found in libexec by the
# frontend's sibling search.
"$SUDO" tee "$PREFIX/bin/Ladybird" >/dev/null <<EOF
#!/bin/sh
unset LD_LIBRARY_PATH
export LADYBIRD_ENGINE_HOST="$PREFIX/libexec/LadybirdEngine"
export LADYBIRD_SOURCE_DIR="\${LADYBIRD_SOURCE_DIR:-$PREFIX}"
export SERENITY_RES_ROOT="\${SERENITY_RES_ROOT:-$PREFIX/share/res}"
exec "$PREFIX/bin/Ladybird.real" "\$@"
EOF
"$SUDO" chmod 0755 "$PREFIX/bin/Ladybird"

echo "==> engine host and helpers"
"$SUDO" cp -a "$LADYBIRD_BUILD/libexec/." "$PREFIX/libexec/"

echo "==> Ladybird libraries"
"$SUDO" cp -a "$LADYBIRD_BUILD/lib/." "$PREFIX/lib/ladybird/"
if [[ -d "$LADYBIRD_BUILD/vcpkg_installed/x64-linux-dynamic/lib" ]]; then
    "$SUDO" cp -a "$LADYBIRD_BUILD/vcpkg_installed/x64-linux-dynamic/lib/." "$PREFIX/lib/ladybird/"
fi

# Helpers are launched with a sanitized environment, so LD_LIBRARY_PATH is not
# enough. Make every installed ELF find its siblings by RPATH.
if command -v patchelf >/dev/null; then
    echo "==> fixing RPATHs"
    while IFS= read -r -d '' binary; do
        "$SUDO" patchelf --force-rpath --set-rpath '$ORIGIN/../lib/ladybird' "$binary"
    done < <(find "$PREFIX/libexec" -maxdepth 1 -type f -perm -u+x -print0)
    while IFS= read -r -d '' library; do
        "$SUDO" patchelf --force-rpath --set-rpath '$ORIGIN' "$library"
    done < <(find "$PREFIX/lib/ladybird" -maxdepth 1 -type f -name '*.so*' -print0)
else
    echo "warning: patchelf is not installed; helper processes may not find Ladybird's libraries" >&2
fi

echo "==> resources"
"$SUDO" cp -a "$LADYBIRD_BUILD/share/Lagom/." "$PREFIX/share/Lagom/"

# The frontend links the SDE's Lagom build. A normal SDE install already has
# these; copying them makes a standalone prefix runnable too.
for library in "$LAGOM"/lib/liblagom-*.so* "$LAGOM"/lib/libwindowserver.so; do
    [[ -e "$library" ]] || continue
    "$SUDO" cp -a "$library" "$PREFIX/lib/"
done

echo "==> desktop entry"
"$SUDO" tee "$PREFIX/share/applications/ladybird.desktop" >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=Ladybird
Comment=A LibGUI frontend for the Ladybird web browser
Exec=$PREFIX/bin/Ladybird
TryExec=$PREFIX/bin/Ladybird
Icon=app-browser
Categories=Network;WebBrowser;
MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;
Terminal=false
StartupNotify=true
StartupWMClass=Ladybird
OnlyShowIn=SDE;
X-SerenityOS=true
EOF

echo "==> installed"
echo "    $PREFIX/bin/Ladybird"
echo "    $PREFIX/libexec/LadybirdEngine"
