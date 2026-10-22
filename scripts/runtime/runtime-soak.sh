#!/usr/bin/env bash
# runtime-soak [iters] (WO-098, SL-ARCH-003 ARCH3-R165, episode 099; in no gate): build sl_runtime_all_tests in the
# debug preset, then run the runtime_stress ctest entries iters times (default 20), each round under a fresh random
# seed (od -An -N8 -tu8 /dev/urandom) passed through SL_RUNTIME_SOAK_SEED, which every stress cell uses instead of its
# fixed seed. On the first failing round it prints the rerun line
#   seed <s>; rerun: SL_RUNTIME_SOAK_SEED=<s> ctest --preset debug -L '^runtime_stress$'
# and exits 1. Exit 2 on usage. Writes only under build/ (the log of each round is build/runtime/soak/round.log).
set -euo pipefail

usage() {
  echo "usage: just runtime-soak [iters]   (iters: a positive integer, default 20)" >&2
  exit 2
}
[[ $# -le 1 ]] || usage
iters=${1:-20}
[[ $iters =~ ^[1-9][0-9]*$ ]] || usage

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

mkdir -p build/runtime/soak
log=build/runtime/soak/round.log

cmake --preset debug
cmake --build --preset debug --target sl_runtime_all_tests

for ((i = 1; i <= iters; ++i)); do
  seed="$(od -An -N8 -tu8 /dev/urandom | tr -d ' \n')"
  echo "runtime-soak: round ${i}/${iters} seed ${seed}"
  set +e
  SL_RUNTIME_SOAK_SEED="$seed" ctest --preset debug -L '^runtime_stress$' --no-tests=error --output-on-failure \
    >"$log" 2>&1
  rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    grep -E 'Failed|seed [0-9]+; rerun' "$log" | head -n 20 >&2 || true
    echo "runtime-soak: FAIL in round ${i} (ctest exit ${rc}); log: ${log}" >&2
    echo "seed ${seed}; rerun: SL_RUNTIME_SOAK_SEED=${seed} ctest --preset debug -L '^runtime_stress\$'"
    exit 1
  fi
done
echo "runtime-soak: ok (${iters} rounds)"
