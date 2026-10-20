#!/usr/bin/env bash
# runtime-contracts (WO-077): every RT-INV id of SL-ARCH-003 bound to one test and one work order; pending-tolerant
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 -I tools/runtime/contracts_check.py "$@"
