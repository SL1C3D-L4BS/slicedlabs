#!/usr/bin/env bash
# runtime-scorecard (WO-100, SL-ARCH-003 ARCH3-R171, RT-INV-GATE-07): build sl_runtime_all_tests with the debug preset
# and rerun runtime_gate_fabric_test, so build/runtime/gate/f5.json comes from a build with the F5 ledger compiled in,
# then tools/runtime/scorecard.py reads build/runtime/gate (demo-report, memreport, f5, stress-matrix and, when present,
# hash-matrix), the sanitizer logs under build/runtime/san (not_run when absent) and the strict contracts summary, and
# writes build/runtime/scorecard.json (sl-runtime-scorecard/1). Exit 1 naming each failed measure. Writes only under
# build/.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_runtime_all_tests >/dev/null
ctest --preset debug -R '^runtime_gate_fabric_test$' --no-tests=error --output-on-failure >/dev/null
exec python3 -I tools/runtime/scorecard.py --out build/runtime/scorecard.json "$@"
