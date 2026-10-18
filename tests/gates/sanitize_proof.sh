#!/usr/bin/env bash
# sanitize-proof (WO-025, SL-EP-050): each sanitizer must catch its planted defect.
# For asan, ubsan and tsan: configure the preset of that name with -DSL_PROOFS=ON into build/neg/proof-<san>, build the
# one proof program of tests/proofs (a planted defect), run it and require a NON-ZERO exit and the sanitizer's report line:
#   asan  asan_overflow  "heap-buffer-overflow"      ubsan  ubsan_shift  "shift exponent"      tsan  tsan_race  "data race"
# ThreadSanitizer needs address-space and ASLR behaviour a jail (bwrap) may deny. When the tsan runtime itself cannot start,
# its exact error is printed and tsan is marked "unproven here"; that alone does not fail the gate. The gate exits 1 when
# asan or ubsan is not proved, or when tsan starts and fails to report the race. SL_BUILD_DIR overrides build/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
base="${SL_BUILD_DIR:-$root/build}/neg"
mkdir -p "$base"

declare -A target=([asan]=asan_overflow [ubsan]=ubsan_shift [tsan]=tsan_race)
declare -A report=([asan]='heap-buffer-overflow' [ubsan]='shift exponent' [tsan]='data race')
declare -A options=(
  [asan]='ASAN_OPTIONS=detect_leaks=0:halt_on_error=1'
  [ubsan]='UBSAN_OPTIONS=halt_on_error=1'
  [tsan]='TSAN_OPTIONS=halt_on_error=0:exitcode=66'
)
# Lines the tsan runtime prints when it cannot run here at all (as opposed to running and finding nothing).
tsan_cannot_run='FATAL: ThreadSanitizer|ThreadSanitizer: failed|unexpected memory mapping|personality|Operation not permitted'

indent() {
  while IFS= read -r line; do printf '  | %s\n' "$line"; done
}

fail=0
summary=()
for san in asan ubsan tsan; do
  dir="$base/proof-$san"
  prog="$dir/tests/proofs/${target[$san]}"
  log="$base/proof-$san.log"
  echo "sanitize-proof: $san: configure, build ${target[$san]}"
  if ! { cmake --preset "$san" -B "$dir" -DSL_PROOFS=ON && cmake --build "$dir" --target "${target[$san]}"; } >"$log" 2>&1; then
    tail -n 20 "$log"
    echo "sanitize-proof: FAIL: $san: the proof did not build (log: $log)"
    fail=1
    summary+=("$san build failed")
    continue
  fi
  set +e
  output="$(env "${options[$san]}" "$prog" 2>&1)"
  status=$?
  set -e
  if [[ $status -ne 0 && $output == *"${report[$san]}"* ]]; then
    echo "sanitize-proof: $san: proved (exit $status, report: $(grep -m1 -F "${report[$san]}" <<<"$output"))"
    summary+=("$san proved")
  elif [[ $san == tsan && $output =~ $tsan_cannot_run ]]; then
    echo "sanitize-proof: tsan: UNPROVEN HERE: the ThreadSanitizer runtime cannot run in this environment (exit $status). Exact error:"
    indent <<<"$output" | head -n 12
    summary+=("tsan unproven here")
  else
    indent <<<"$output" | head -n 15
    echo "sanitize-proof: FAIL: $san: exit $status, expected a non-zero exit and a report containing '${report[$san]}'"
    fail=1
    summary+=("$san FAILED")
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "sanitize-proof: FAIL (${summary[*]})"
  exit 1
fi
echo "sanitize-proof: ok (${summary[*]})"
