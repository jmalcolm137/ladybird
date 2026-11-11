#!/usr/bin/env bash
# Smoke the frontend fuzzer against the live LibGUI build.
#
# Covers the protocol mutation corpus, the window/dialog lifecycle sequence,
# the modal corpus, and the compositor's close button. A full run is much
# longer; this is the CI-sized slice.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FUZZ="$ROOT/scripts/fuzz-frontend.py"

[[ -x "$FUZZ" ]] || { echo "error: $FUZZ is not executable" >&2; exit 1; }

echo "== protocol fuzz"
"$FUZZ" --seeds 1-3 --duration 4

echo "== lifecycle"
"$FUZZ" --scenario lifecycle --seed 1 --duration 12

echo "== modal fuzz"
"$FUZZ" --scenario modals --seed 1 --duration 6

echo "== close request"
"$FUZZ" --scenario close --seed 1 --duration 8

echo "== disconnect"
"$FUZZ" --scenario disconnect --seed 1 --duration 6

echo "== multi-window"
"$FUZZ" --scenario windows --seed 1 --duration 6

echo "== resize"
"$FUZZ" --scenario resize --seed 1 --duration 6

echo "== edge resize"
"$FUZZ" --scenario resize-edge --seed 1 --duration 6

echo "== session restore"
"$FUZZ" --scenario session --seed 1 --duration 8

echo "==> frontend fuzz smoke PASSED"
