#!/usr/bin/env bash
# Build the LibGUI frontend against the Lagom LibGUI used by the SDE.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec make -C "$ROOT/UI/LibGUI" -j"${JOBS:-$(nproc)}" "$@"
