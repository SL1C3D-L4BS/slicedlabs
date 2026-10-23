#!/usr/bin/env bash
# runtime-gate (WO-101, SL-ARCH-003 ARCH3-R172, ADR-0028): the release gate of the runtime foundation (episode 100).
# Runs, each through `just` and in this order, and stops at the first failure naming the recipe:
#   runtime-lint, runtime-contracts-strict, runtime-contract-lock, runtime-test <area> for every test area,
#   runtime-stress, runtime-demo, runtime-memreport-check, runtime-scorecard, fabric-surface-check, and every
#   neg-runtime-* recipe (read from `just --summary`, so a new one is composed without editing this file).
# The areas are the file-name prefixes of tests/unit/runtime and tests/integration/runtime, minus stress (runtime-stress
# runs that area and writes its matrix). runtime-memreport-check runs after runtime-demo, not before as the order's
# brief lists it, because its default input build/runtime/gate/memreport.json is written by the demo.
# Sanitizer logs and hash-matrix.json are check-full evidence (runtime-san, runtime-determinism): the gate removes the
# copies left by an earlier run so the scorecard reports them not_run instead of reading stale files.
# Wall time: runtime-demo takes about 500 s in Debug, so the gate takes a quarter hour; each step and the total are
# printed and recorded in docs/episodes/081-100.md, never asserted. Prints "runtime-gate: ok" last. Exit 2 on usage,
# 1 when a step fails. Writes only under build/.
set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: just runtime-gate" >&2
  exit 2
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

mapfile -t areas < <(
  find tests/unit/runtime tests/integration/runtime -maxdepth 1 -name '*_test.cpp' -printf '%f\n' |
    sed 's/_.*//' | LC_ALL=C sort -u | grep -v '^stress$' || true
)
mapfile -t negs < <(just --summary | tr ' ' '\n' | grep '^neg-runtime-' || true)
if [[ ${#areas[@]} -eq 0 || ${#negs[@]} -eq 0 ]]; then
  echo "runtime-gate: FAIL: found ${#areas[@]} test areas and ${#negs[@]} neg-runtime-* recipes, expected both non-empty" >&2
  exit 1
fi

rm -f build/runtime/gate/hash-matrix.json build/runtime/san/*.log

total_start=$SECONDS
step() {
  local start=$SECONDS rc=0
  echo "runtime-gate: just $*"
  just "$@" || rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "runtime-gate: FAIL: just $* exited $rc" >&2
    exit 1
  fi
  echo "runtime-gate: ok: just $* ($((SECONDS - start)) s)"
}

step runtime-lint
step runtime-contracts-strict
step runtime-contract-lock
for area in "${areas[@]}"; do
  step runtime-test "$area"
done
step runtime-stress
step runtime-demo
step runtime-memreport-check
step runtime-scorecard
step fabric-surface-check
for neg in "${negs[@]}"; do
  step "$neg"
done

echo "runtime-gate: ${#areas[@]} test areas, ${#negs[@]} negative tests, $((SECONDS - total_start)) s wall"
echo "runtime-gate: ok"
