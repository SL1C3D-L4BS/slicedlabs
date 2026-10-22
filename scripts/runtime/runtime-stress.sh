#!/usr/bin/env bash
# runtime-stress (WO-098, SL-ARCH-003 ARCH3-R162 to R166, episode 099): configure the debug preset, build only
# sl_runtime_all_tests, run the ctest entries labelled runtime_stress (tests/integration/runtime/stress_*_test.cpp,
# RUN_SERIAL) with JUnit output to build/runtime/stress/junit.xml, then write build/runtime/gate/stress-matrix.json
# (sl-runtime-stress-matrix/1) through tools/runtime/stress_report.py. Exit: ctest's exit when it failed, else the
# report's (1 on a missing cell or a schema finding). Writes only under build/.
set -euo pipefail

[[ $# -eq 0 ]] || {
  echo "usage: just runtime-stress" >&2
  exit 2
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

junit=build/runtime/stress/junit.xml
mkdir -p build/runtime/stress build/runtime/gate
rm -f "$junit"

cmake --preset debug
cmake --build --preset debug --target sl_runtime_all_tests

set +e
ctest --preset debug -L '^runtime_stress$' --no-tests=error --output-on-failure --output-junit "$root/$junit"
rc=$?
set -e

if [[ ! -f $junit ]]; then
  echo "runtime-stress: FAIL: ctest wrote no ${junit} (ctest exit ${rc})" >&2
  exit 1
fi
set +e
python3 -I tools/runtime/stress_report.py --junit "$junit" --out build/runtime/gate/stress-matrix.json --preset debug
report=$?
set -e

if [[ $rc -ne 0 ]]; then
  echo "runtime-stress: FAIL: ctest exited ${rc}; the matrix in build/runtime/gate/stress-matrix.json names the cells" >&2
  exit "$rc"
fi
if [[ $report -ne 0 ]]; then
  echo "runtime-stress: FAIL: stress_report.py exited ${report}" >&2
  exit "$report"
fi
echo "runtime-stress: ok; build/runtime/gate/stress-matrix.json"
