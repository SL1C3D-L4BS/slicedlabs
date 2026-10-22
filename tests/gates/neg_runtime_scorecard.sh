#!/usr/bin/env bash
# neg-runtime-scorecard (WO-100, ARCH3-R171 RT-INV-GATE-07): tools/runtime/scorecard.py must fail naming each bad
# measure. Builds its own inputs from the schema examples' shapes, so it needs no prior gate run:
#   control: a clean gate directory (memreport leaks [], demo recovery equal, F5 enabled 0 of 37, stress S1-S10 pass,
#            no hash matrix) and a sanitizer directory with one clean log -> exit 0, divergences cells not_run-free
#   (a) the copy gets one leak entry in memreport.json and one AddressSanitizer report line in asan-mem.log: the
#       scorecard exits 1 and names both leaks and sanitizer_reports
# Invariant coverage comes from the repository's strict contracts run. Works under build/runtime/neg/scorecard
# (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
scorecard="$root/tools/runtime/scorecard.py"
work="$build/runtime/neg/scorecard"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-scorecard: FAIL: $1"
  exit 1
}

fresh() {
  rm -rf "$work"
  mkdir -p "$work/gate" "$work/san"
  cat >"$work/gate/memreport.json" <<'EOF'
{"format": "sl-runtime-memreport/1", "instance": 1, "frame": 600, "tags": [], "allocators": [], "external": [],
 "leaks": [], "upstream_allocs_last_frame": 0}
EOF
  cat >"$work/gate/demo-report.json" <<'EOF'
{"format": "sl-runtime-gate/1", "recovery": {"fault_tick": 450, "restored_tick": 300, "hash_equal": true}}
EOF
  cat >"$work/gate/f5.json" <<'EOF'
{"format": "sl-runtime-f5/1", "mutations": 37, "bypassing": 0, "host_commits": 30, "enabled": true}
EOF
  {
    printf '{"format": "sl-runtime-stress-matrix/1", "scale": 1.0, "cells": ['
    for n in 1 2 3 4 5 6 7 8 9 10; do
      [ "$n" -eq 1 ] || printf ', '
      printf '{"cell": "S%d", "preset": "debug", "test": "t", "pass": true, "wall_s": 1.0, "seed": %d}' "$n" "$n"
    done
    printf ']}\n'
  } >"$work/gate/stress-matrix.json"
  printf 'test 1\n1: [doctest] Status: SUCCESS!\n' >"$work/san/asan-mem.log"
}

# run_card: sets out and status
run_card() {
  set +e
  out="$(python3 -I "$scorecard" --root "$root" --gate "$work/gate" --san "$work/san" --out "$work/scorecard.json" 2>&1)"
  status=$?
  set -e
}

echo "neg-runtime-scorecard: control run (clean inputs)"
fresh
run_card
[ "$status" -eq 0 ] || {
  echo "$out"
  fail "control: the scorecard must exit 0 on clean inputs"
}
grep -qE '^divergences +pass' <<<"$out" || fail "control: divergences must pass on the recovery hash alone"
grep -qF '"status": "pass"' "$work/scorecard.json" || fail "control: scorecard.json has no passing measure"

fresh
python3 -I - "$work/gate/memreport.json" <<'EOF'
import json, sys
path = sys.argv[1]
doc = json.load(open(path))
doc["leaks"] = [{"tag": "ecs.chunks", "site": "planted.cpp:1", "size": 32}]
json.dump(doc, open(path, "w"))
EOF
printf '==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x1 (planted)\n' >>"$work/san/asan-mem.log"
run_card
[ "$status" -eq 1 ] || {
  echo "$out"
  fail "a-leak-and-report: the scorecard exited $status, expected 1"
}
for measure in leaks sanitizer_reports; do
  grep -qE "^FAIL $measure: " <<<"$out" || {
    echo "$out"
    fail "a-leak-and-report: no FAIL line names $measure"
  }
done
grep -qF '"sanitizer_reports": {' "$work/scorecard.json" || fail "a-leak-and-report: scorecard.json lacks sanitizer_reports"
echo "neg-runtime-scorecard: a-leak-and-report -> exit $status, FAIL lines name leaks and sanitizer_reports"

echo "neg-runtime-scorecard: ok (control exit 0, the planted leak and sanitizer report each named)"
