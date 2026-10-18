#!/usr/bin/env bash
# ci-local (WO-020): the whole local CI in one command, from the repository root. No network.
# Usage: scripts/ci-local.sh [--quick | --full]
#   default  doctor, fmt-check, gov-check, then cmake --workflow --preset ci-clang, ci-gcc, ci-asan
#   --quick  doctor, fmt-check, gov-check, ci-clang only
#   --full   default plus ci-ubsan, ci-tsan, ci-release, ci-gcc-release. A workflow preset that CMakePresets.json
#            does not define is skipped with a visible "skipped: no workflow preset <name>" line (summary and
#            the final line list them); the run still exits 0 when every step that did run passed (WO-028).
# Each step is timed; output goes to build/ci-local/<step>.log and the tail is printed on failure.
# The first failing step stops the run: a summary table is printed, the step is named, exit 1.
# Exit codes: 0 all steps passed, 1 a step failed, 2 usage error.
# SL_DEPS_PROVIDERS (and every other environment variable) is passed through unchanged.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mode=default
case "${1:-}" in
  "") ;;
  --quick) mode=quick ;;
  --full) mode=full ;;
  *)
    echo "usage: scripts/ci-local.sh [--quick | --full]" >&2
    exit 2
    ;;
esac
if [ "$#" -gt 1 ]; then
  echo "usage: scripts/ci-local.sh [--quick | --full]" >&2
  exit 2
fi

logdir="$root/build/ci-local"
rm -rf "$logdir"
mkdir -p "$logdir"

names=()
results=()
times=()
total_start=$SECONDS

print_summary() {
  local i
  echo
  echo "ci-local summary ($mode)"
  printf '  %-26s %-8s %s\n' step result seconds
  for i in "${!names[@]}"; do
    printf '  %-26s %-8s %s\n' "${names[$i]}" "${results[$i]}" "${times[$i]}"
  done
  echo "  total wall: $((SECONDS - total_start)) s"
}

# fail_step <name> <log>: record, print the summary and the log tail, exit 1
fail_step() {
  local name="$1" log="$2" phase=""
  if [ -f "$log" ]; then
    phase="$(grep -E '^Executing workflow step' "$log" | tail -n 1 || true)"
  fi
  print_summary
  echo
  echo "ci-local: FAILED at step '$name'${phase:+ ($phase)}"
  if [ -f "$log" ]; then
    echo "ci-local: last lines of $log"
    tail -n 60 "$log"
  fi
  exit 1
}

# run_step <name> <command...>
run_step() {
  local name="$1" start rc=0 log
  shift
  log="$logdir/${name// /_}.log"
  echo "ci-local: [$name] ..."
  start=$SECONDS
  "$@" >"$log" 2>&1 || rc=$?
  names+=("$name")
  times+=("$((SECONDS - start))")
  if [ "$rc" -ne 0 ]; then
    results+=("FAIL")
    fail_step "$name" "$log"
  fi
  results+=("ok")
  echo "ci-local: [$name] ok (${times[-1]} s)"
}

skipped_presets=()
skip_step() {
  names+=("$1")
  results+=("skipped")
  times+=("-")
  echo "ci-local: [$1] skipped: $2"
}

workflows="$(cmake --list-presets=workflow 2>/dev/null || true)"
has_workflow() { printf '%s\n' "$workflows" | grep -q "\"$1\""; }

run_step doctor bash scripts/doctor
run_step fmt-check just fmt-check
run_step gov-check just gov-check

case "$mode" in
  quick) presets=(ci-clang) ;;
  full) presets=(ci-clang ci-gcc ci-asan ci-ubsan ci-tsan ci-release ci-gcc-release) ;;
  *) presets=(ci-clang ci-gcc ci-asan) ;;
esac
for p in "${presets[@]}"; do
  if has_workflow "$p"; then
    run_step "workflow $p" cmake --workflow --preset "$p"
  else
    skip_step "workflow $p" "no workflow preset $p"
    skipped_presets+=("$p")
  fi
done

print_summary
if [ "${#skipped_presets[@]}" -gt 0 ]; then
  echo "ci-local: skipped: no workflow preset for: ${skipped_presets[*]}"
fi
echo "ci-local: ok ($mode)"
