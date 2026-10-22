#!/usr/bin/env bash
# runtime-demo [flags] (WO-099, SL-ARCH-003 ARCH3-R167 and R168, ADR-0028): configure the debug preset, build
# sl_runtime_demo, run the lantern-field scene with --evidence-dir build/runtime/gate plus the given flags (--seed,
# --ticks, --workers, --mode, --entities, --snapshot-every), tee its output to build/runtime/gate/demo.log (the runner
# then replaces that file with its own identical log) and require "runtime-demo: ok" as the last line. Exit: the demo's
# exit code (2 usage, 1 divergence or invariant failure, 3 runtime fault), 1 when it exits 0 without the ok line.
# Writes only under build/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
gate=build/runtime/gate

cmake --preset debug
cmake --build --preset debug --target sl_runtime_demo
mkdir -p "$gate"
set +e
build/debug/engine/runtime/tools/sl_runtime_demo --evidence-dir "$gate" "$@" | tee "$gate/demo.log"
code=${PIPESTATUS[0]}
set -e
if [[ $code -ne 0 ]]; then
  echo "runtime-demo: FAIL: sl_runtime_demo exit $code" >&2
  exit "$code"
fi
last="$(tail -n 1 "$gate/demo.log")"
if [[ $last != "runtime-demo: ok" ]]; then
  echo "runtime-demo: FAIL: the last line is not 'runtime-demo: ok'" >&2
  exit 1
fi
