#!/usr/bin/env bash
# Snapshot the built browser, engine host, helpers, and the libraries they map into a
# stable directory. Live sessions run this snapshot, so rebuilding the source/build
# tree cannot replace a helper or a library underneath a running browser.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIVE="${LADYBIRD_LIVE_DIR:-$ROOT/build/live}"
LAGOM="${LAGOM:-$ROOT/../libgui-wayland/build/lagom}"

if [[ ! -x "$ROOT/build/chrome/Ladybird" ]]; then
    echo "error: $ROOT/build/chrome/Ladybird is missing; build the frontend first" >&2
    exit 1
fi
if [[ ! -x "$ROOT/Build/release/libexec/LadybirdEngine" ]]; then
    echo "error: $ROOT/Build/release/libexec/LadybirdEngine is missing; build the engine first" >&2
    exit 1
fi

TMP="${LIVE}.tmp.$$"
trap 'rm -rf "$TMP"' EXIT
rm -rf "$TMP"
mkdir -p "$TMP/bin" "$TMP/libexec" "$TMP/lib" "$TMP/lib/vcpkg" "$TMP/libgui" "$TMP/share"

# $ORIGIN/../lib for the browser and every helper resolves to the snapshot's lib.
install -m 0755 "$ROOT/build/chrome/Ladybird" "$TMP/bin/Ladybird"
cp -a "$ROOT/Build/release/libexec/." "$TMP/libexec/"
cp -a "$ROOT/Build/release/lib/." "$TMP/lib/"
# The resource root is derived from the executable path ($prefix/share/Lagom). It holds the
# theme, internal pages, and the site-compatibility data the engine loads at startup.
if [[ -d "$ROOT/Build/release/share/Lagom" ]]; then
    cp -a "$ROOT/Build/release/share/Lagom" "$TMP/share/"
fi
if [[ -d "$ROOT/Build/release/vcpkg_installed/x64-linux-dynamic/lib" ]]; then
    cp -a "$ROOT"/Build/release/vcpkg_installed/x64-linux-dynamic/lib/*.so* "$TMP/lib/vcpkg/"
fi

# The chrome also maps the LibGUI/LibWM framework. Keep its own copy so a framework
# rebuild cannot change the libraries a running session maps.
if [[ -d "$LAGOM/lib" ]]; then
    cp -a "$LAGOM"/lib/*.so* "$TMP/libgui/"
fi

# Make the snapshot self-contained. The browser loads the LibGUI/LibWM framework from
# libgui first; the engine and its helpers load the engine's libraries from lib.
if command -v patchelf >/dev/null; then
    patchelf --set-rpath '$ORIGIN/../libgui:$ORIGIN/../lib:$ORIGIN/../lib/vcpkg' "$TMP/bin/Ladybird"
    while IFS= read -r executable; do
        [[ "$(file -b "$executable")" == *ELF* ]] || continue
        patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/vcpkg' "$executable"
    done < <(find "$TMP/libexec" -maxdepth 1 -type f -perm -111)

    for library in "$TMP"/lib/*.so*; do
        [[ "$(file -b "$library")" == *ELF* ]] || continue
        patchelf --set-rpath '$ORIGIN:$ORIGIN/vcpkg' "$library"
    done
    for library in "$TMP"/libgui/*.so*; do
        [[ "$(file -b "$library")" == *ELF* ]] || continue
        patchelf --set-rpath '$ORIGIN' "$library"
    done
fi

rm -rf "$LIVE"
mv "$TMP" "$LIVE"
trap - EXIT

echo "==> live snapshot: $LIVE"
echo "    browser : $LIVE/bin/Ladybird"
echo "    engine  : $LIVE/libexec/LadybirdEngine"
echo "    libs    : $LIVE/lib, $LIVE/libgui"
echo "    res     : $LIVE/share/Lagom"
echo "    Restart the browser to use it."
