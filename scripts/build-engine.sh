#!/usr/bin/env bash
# Build the Ladybird engine host (the headless UI process that owns WebContent).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/Meta/ladybird.py" build --preset "${LADYBIRD_PRESET:-Release}" LadybirdEngine "$@"
