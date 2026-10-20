#!/usr/bin/env bash
# runtime-test <area> (WO-076, SL-ARCH-003 ARCH3-R014/R016): configure the debug preset, build only
# sl_runtime_all_tests and run the ctest entries labelled runtime_<area> (tests/unit/runtime and
# tests/integration/runtime files whose name starts with <area>_). Exit 2 on usage, ctest's exit otherwise; a label with
# no test is an error. Writes only under build/.
set -euo pipefail

usage() {
  echo "usage: just runtime-test <area>   (area: lower-case letters, the test file prefix, e.g. err or flt)" >&2
  exit 2
}
[[ $# -eq 1 && $1 =~ ^[a-z]+$ ]] || usage
area=$1

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

cmake --preset debug
cmake --build --preset debug --target sl_runtime_all_tests
ctest --preset debug -L "^runtime_${area}\$" --no-tests=error --output-on-failure
