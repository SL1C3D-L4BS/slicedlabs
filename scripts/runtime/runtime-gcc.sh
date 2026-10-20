#!/usr/bin/env bash
# runtime-gcc [area] (WO-076, SL-ARCH-003 ARCH3-R016): configure the ci-gcc preset (gcc, -Werror with -Wshadow and
# -Wsign-conversion), build only sl_runtime, sl_runtime_testing, sl_runtime_all_tests and the runtime tools
# (engine/runtime/tools/sl_*.cpp), then run the ctest entries labelled runtime_<area> (or every runtime entry) under
# ci-gcc. Exit 2 on usage, the failing step's exit otherwise. Writes only under build/.
set -euo pipefail

usage() {
  echo "usage: just runtime-gcc [area]   (area: lower-case letters, e.g. flt)" >&2
  exit 2
}
[[ $# -le 1 ]] || usage
area=${1:-}
[[ -z $area || $area =~ ^[a-z]+$ ]] || usage

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

targets=(sl_runtime sl_runtime_testing sl_runtime_all_tests)
for src in engine/runtime/tools/sl_*.cpp; do
  [[ -f $src ]] || continue
  name=${src##*/}
  targets+=("${name%.cpp}")
done
if [[ -n $area ]]; then
  label="^runtime_${area}\$"
else
  label='^runtime$'
fi

cmake --preset ci-gcc
cmake --build --preset ci-gcc --target "${targets[@]}"
ctest --preset ci-gcc -L "$label" --no-tests=error --output-on-failure
