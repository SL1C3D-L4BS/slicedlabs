#!/usr/bin/env bash
# runtime-determinism (WO-099, SL-ARCH-003 ARCH3-R085 and R170, ADR-0026): build sl_runtime_demo with the gcc-release
# and debug presets, run the gate scene (seed 1, 600 ticks, 50000 lanterns) with the gcc-release demo for workers
# {1, 2, 4, 8, 16} x mode {parallel, serial}, each run checking its own resume (from the tick-300 snapshot) and replay
# (of its recorded inputs) against its fresh pass, into build/runtime/gate/runs/gcc-release-w<N>-<mode>/; run the
# debug (clang) demo once (workers 4, parallel) into build/runtime/gate/runs/debug-w4-parallel/ for the cross-compiler
# record; then tools/runtime/determinism_check.py writes build/runtime/gate/hash-matrix.json and fails on the first
# divergent (workers, mode, variant, tick) or a missing cell. The clang-versus-gcc comparison never sets the exit code.
# Exit 2 on usage, 1 when a run fails or the matrix diverges, the failing build step's exit otherwise. Writes only
# under build/.
set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: just runtime-determinism" >&2
  exit 2
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
gate=build/runtime/gate
runs=build/runtime/gate/runs

cmake --preset gcc-release
cmake --build --preset gcc-release --target sl_runtime_demo
cmake --preset debug
cmake --build --preset debug --target sl_runtime_demo

rm -rf "$runs"
mkdir -p "$runs"
failed=0
run() {
  local build=$1 workers=$2 mode=$3
  local dir="$runs/$build-w$workers-$mode"
  local start=$SECONDS
  set +e
  "build/$build/engine/runtime/tools/sl_runtime_demo" --workers "$workers" --mode "$mode" --evidence-dir "$dir" \
    >"$runs/$build-w$workers-$mode.log" 2>&1
  local code=$?
  set -e
  echo "runtime-determinism: $build workers=$workers mode=$mode exit $code ($((SECONDS - start)) s)"
  if [[ $code -ne 0 ]]; then
    tail -n 3 "$runs/$build-w$workers-$mode.log"
    failed=1
  fi
}
for workers in 1 2 4 8 16; do
  for mode in parallel serial; do
    run gcc-release "$workers" "$mode"
  done
done
run debug 4 parallel

set +e
python3 -I tools/runtime/determinism_check.py --runs "$runs" --primary gcc-release --out "$gate/hash-matrix.json"
code=$?
set -e
if [[ $code -ne 0 ]]; then
  exit "$code"
fi
if [[ $failed -ne 0 ]]; then
  echo "runtime-determinism: FAIL: a demo run did not exit 0" >&2
  exit 1
fi
echo "runtime-determinism: ok"
