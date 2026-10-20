#!/usr/bin/env bash
# runtime-tidy [area] (WO-076, SL-ARCH-003): configure the debug preset, build sl_runtime_all_tests, then run
# run-clang-tidy -p build/debug -quiet over the area's files: tests/{unit,integration}/runtime/<area>_*.cpp plus the
# area's runtime sources from the table below; no area means every .cpp of engine/runtime and tests/*/runtime. Files
# that do not exist yet are skipped; an area with no file at all is an error (exit 1), because tidy over nothing proves
# nothing. Exit 2 on usage, run-clang-tidy's exit otherwise. Writes only under build/.
set -euo pipefail

usage() {
  echo "usage: just runtime-tidy [area]   (area: lower-case letters, e.g. flt)" >&2
  exit 2
}
[[ $# -le 1 ]] || usage
area=${1:-}
[[ -z $area || $area =~ ^[a-z]+$ ]] || usage

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

# Area -> runtime sources: names relative to engine/runtime/src unless they start with testing/ or tools/ (relative to
# engine/runtime); a name without .cpp is a prefix pattern.
declare -A sources=(
  [err]='error.cpp faults.cpp'
  [flt]='faults.cpp'
  [plat]='platform_*'
  [mem]='memory_*'
  [sync]='sync_*'
  [alloc]='alloc_*'
  [job]='job_system* queue_locked policy_simple'
  [ent]='ecs_entity*'
  [clock]='time_*'
  [det]='rng* det_*'
  [diag]='memdiag_*'
  [steal]='queue_ws policy_steal*'
  [refl]='reflect_*'
  [store]='ecs_world* ecs_archetype* ecs_component*'
  [frame]='frame_*'
  [query]='ecs_query*'
  [cmd]='ecs_command*'
  [evt]='event_*'
  [chg]='ecs_change*'
  [sched]='ecs_system* ecs_access_guard'
  [snap]='snapshot_*'
  [life]='runtime_*'
  [bnd]='bridge_*'
  [stress]='testing/stress_harness.cpp'
  [gate]='tools/sl_runtime_demo.cpp tools/demo/*.cpp'
)

shopt -s nullglob
files=()
if [[ -z $area ]]; then
  while IFS= read -r -d '' f; do
    files+=("$f")
  done < <(find engine/runtime tests/unit/runtime tests/integration/runtime -name '*.cpp' -print0 | sort -z)
else
  files+=(tests/unit/runtime/"${area}"_*.cpp tests/integration/runtime/"${area}"_*.cpp)
  read -r -a patterns <<<"${sources[$area]:-}"
  for p in "${patterns[@]}"; do
    case $p in
      testing/* | tools/*) dir=engine/runtime ;;
      *) dir=engine/runtime/src ;;
    esac
    [[ $p == *.cpp ]] || p="${p}.cpp"
    for f in "$dir"/$p; do
      files+=("$f")
    done
  done
fi
shopt -u nullglob
existing=()
for f in "${files[@]}"; do
  [[ -f $f ]] && existing+=("$f")
done
if [[ ${#existing[@]} -eq 0 ]]; then
  echo "runtime-tidy: FAIL: no runtime source or test file for area '${area}'" >&2
  exit 1
fi

cmake --preset debug
cmake --build --preset debug --target sl_runtime_all_tests

# run-clang-tidy takes regexes on the compile-database path: anchor each absolute path and escape its dots.
regexes=()
for f in "${existing[@]}"; do
  abs="${root}/${f}"
  regexes+=("^${abs//./\\.}\$")
done
printf 'runtime-tidy: %s\n' "${existing[@]}"
run-clang-tidy -p build/debug -quiet -j "$(nproc)" "${regexes[@]}"
echo "runtime-tidy: ok (${#existing[@]} files, area ${area:-all})"
