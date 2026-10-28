#!/usr/bin/env bash
# Build both halves of the LibGUI Ladybird.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build-engine.sh" "$@"
"$ROOT/scripts/build-chrome.sh"
