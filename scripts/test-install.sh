#!/usr/bin/env bash
# Deployment regression test: install into a disposable prefix and assert the
# frontend, engine, resources, app file, icon, and desktop entry land where the
# SDE expects them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${1:-$(mktemp -d "${TMPDIR:-/tmp}/ladybird-install-test.XXXXXX")}"
OWN_PREFIX=0
if [[ $# -eq 0 ]]; then
    OWN_PREFIX=1
fi

cleanup() {
    if [[ "$OWN_PREFIX" == 1 ]]; then
        rm -rf "$PREFIX"
    fi
}
trap cleanup EXIT

SUDO=env SDE_PREFIX="$PREFIX" "$ROOT/scripts/install-sde.sh" >/dev/null

for path in \
    "$PREFIX/bin/Ladybird" \
    "$PREFIX/bin/Ladybird.real" \
    "$PREFIX/libexec/LadybirdEngine" \
    "$PREFIX/share/res/apps/Ladybird.af" \
    "$PREFIX/share/res/icons/32x32/app-browser.png" \
    "$PREFIX/share/applications/ladybird.desktop"; do
    [[ -e "$path" ]] || { echo "  FAIL: missing $path"; exit 1; }
done

grep -q "Executable=$PREFIX/bin/Ladybird" "$PREFIX/share/res/apps/Ladybird.af" || {
    echo "  FAIL: the app file does not point at the installed launcher"
    exit 1
}
grep -q "Exec=$PREFIX/bin/Ladybird" "$PREFIX/share/applications/ladybird.desktop" || {
    echo "  FAIL: the desktop entry does not point at the installed launcher"
    exit 1
}

echo "  ok: installed frontend, engine, resources, app file, icon, and desktop entry"
echo "==> deployment test PASSED"
