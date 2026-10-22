#!/usr/bin/env bash
# runtime-contract-lock (WO-100, SL-ARCH-003 ARCH3-R020, RT-INV-GATE-06): build sl_runtime_dump_contract with the debug
# preset, write its contract dump to build/runtime/contract-dump.json, then tools/runtime/contract_lock.py compares it
# with every sl-data table of SL-ARCH-003 and with schemas/runtime/contract.lock.json, printing each differing key path
# (exit 1 on any). Writes only under build/; `python3 -I tools/runtime/contract_lock.py write` regenerates the lock and
# no acceptance command runs it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_runtime_dump_contract >/dev/null
mkdir -p build/runtime
build/debug/engine/runtime/tools/sl_runtime_dump_contract >build/runtime/contract-dump.json
exec python3 -I tools/runtime/contract_lock.py check build/runtime/contract-dump.json "$@"
