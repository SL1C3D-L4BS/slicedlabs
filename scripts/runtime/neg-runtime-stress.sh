#!/usr/bin/env bash
# neg-runtime-stress (WO-098): an event_count without its notify epoch increment must lose a wakeup and be caught
# (tests/gates/neg_runtime_stress.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_stress.sh "$@"
