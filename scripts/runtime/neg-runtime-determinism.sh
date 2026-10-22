#!/usr/bin/env bash
# neg-runtime-determinism (WO-099): completion-order command playback must fail the D1 hash matrix
# (tests/gates/neg_runtime_determinism.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_determinism.sh "$@"
