#!/usr/bin/env bash
# Run the browser-feature regression set: engine side plus native chrome side.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/scripts/test-engine-features.py"
"$ROOT/scripts/test-spoofing-ui.sh"
"$ROOT/scripts/test-inline-completion.sh"
"$ROOT/scripts/test-drag-source.sh"
"$ROOT/scripts/test-compat-report.sh"
