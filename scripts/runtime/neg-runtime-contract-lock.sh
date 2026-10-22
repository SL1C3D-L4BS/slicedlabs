#!/usr/bin/env bash
# neg-runtime-contract-lock (WO-100): an errc number drifted in a copy of SL-ARCH-003, then in a copy of the lock, must
# fail runtime-contract-lock naming the code (tests/gates/neg_runtime_contract_lock.sh)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec bash tests/gates/neg_runtime_contract_lock.sh "$@"
