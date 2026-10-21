#!/usr/bin/env bash
# neg-runtime-leak (WO-085): a memory report with one planted leak must fail runtime-memreport-check --no-leaks, naming
# the leak's tag and site (tests/gates/neg_runtime_leak.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_leak.sh "$@"
