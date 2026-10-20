#!/usr/bin/env bash
# neg-runtime-race (WO-082): a work queue without its lock must yield a ThreadSanitizer report (tests/gates/neg_runtime_race.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_race.sh "$@"
