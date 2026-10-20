#!/usr/bin/env bash
# runtime-san <asan|ubsan|tsan> [area] (WO-076, SL-ARCH-003 ARCH3-R016, ADR-0024): configure the ci-<san> preset, build
# only sl_runtime_all_tests (and sl_runtime_demo for area gate), run the ctest entries labelled runtime_<area> (or every
# runtime entry) verbosely and tee the output to build/runtime/san/<san>-<area|all>.log. Fail-closed: a non-zero ctest,
# a sanitizer runtime that fails to start, or any sanitizer report line in the log exits 1 naming the log. Exit 2 on
# usage. Writes only under build/.
set -euo pipefail

usage() {
  echo "usage: just runtime-san <asan|ubsan|tsan> [area]   (area: lower-case letters, e.g. flt)" >&2
  exit 2
}
[[ $# -ge 1 && $# -le 2 ]] || usage
san=$1
area=${2:-}
[[ $san =~ ^(asan|ubsan|tsan)$ ]] || usage
[[ -z $area || $area =~ ^[a-z]+$ ]] || usage

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

preset="ci-${san}"
targets=(sl_runtime_all_tests)
if [[ $area == gate ]]; then
  targets+=(sl_runtime_demo)
fi
if [[ -n $area ]]; then
  label="^runtime_${area}\$"
else
  label='^runtime$'
fi

log="build/runtime/san/${san}-${area:-all}.log"
mkdir -p build/runtime/san

cmake --preset "$preset"
cmake --build --preset "$preset" --target "${targets[@]}"

set +e
ctest --preset "$preset" -L "$label" --no-tests=error --output-on-failure -V 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -ne 0 ]]; then
  echo "runtime-san: FAIL: ctest exited ${rc} under ${preset} (a failed test or a sanitizer that did not start); log: ${log}" >&2
  exit 1
fi
if grep -En 'ThreadSanitizer:|AddressSanitizer:|LeakSanitizer:|runtime error:' "$log" >&2; then
  echo "runtime-san: FAIL: sanitizer report in ${log}" >&2
  exit 1
fi
echo "runtime-san: ok (${san}, ${area:-all}); log: ${log}"
