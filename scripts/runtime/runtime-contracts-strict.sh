#!/usr/bin/env bash
# runtime-contracts-strict (WO-077): runtime-contracts where a missing test or a stub verifier is an error (WO-100 gate)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 -I tools/runtime/contracts_check.py --strict "$@"
