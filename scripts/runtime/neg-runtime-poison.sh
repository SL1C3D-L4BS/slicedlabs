#!/usr/bin/env bash
# neg-runtime-poison (WO-081): a read of arena memory after reset() must yield an ASan use-after-poison (tests/gates/neg_runtime_poison.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_poison.sh "$@"
