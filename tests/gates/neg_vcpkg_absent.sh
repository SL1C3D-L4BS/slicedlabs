#!/usr/bin/env bash
# neg-vcpkg-absent (WO-019): without vcpkg the vcpkg provider and preset refuse, and say what the founder installs.
# Every run unsets VCPKG_ROOT (env -u) so a vcpkg on the host cannot change the result.
#   1. control: `cmake --preset debug -DSL_DEPS_PROVIDERS=vendored` configures (exit 0)
#   2. `cmake --preset vcpkg-debug` must exit non-zero (the preset is disabled while VCPKG_ROOT is empty)
#   3. `cmake --preset debug -DSL_DEPS_PROVIDERS=vcpkg` must exit non-zero and say "vcpkg toolchain not active" and
#      the founder install step `sudo pacman -S vcpkg`
# Build directories live under ${SL_BUILD_DIR:-build}/neg/vcpkg-absent and are removed on exit.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/vcpkg-absent"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-vcpkg-absent: FAIL: $1"
  exit 1
}

echo "neg-vcpkg-absent: control run (VCPKG_ROOT unset, SL_DEPS_PROVIDERS=vendored)"
env -u VCPKG_ROOT cmake --preset debug -B "$work/control" -DSL_DEPS_PROVIDERS=vendored >"$work/control.log" 2>&1 || {
  tail -n 12 "$work/control.log"
  fail "control run must configure but failed"
}

echo "neg-vcpkg-absent: planted run 1 (cmake --preset vcpkg-debug)"
set +e
out="$(env -u VCPKG_ROOT cmake --preset vcpkg-debug -B "$work/preset" 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | head -n 3
[ "$status" -ne 0 ] || fail "vcpkg-debug configured with VCPKG_ROOT unset (exit 0)"
printf '%s\n' "$out" | grep -qi 'vcpkg-debug' || fail "vcpkg-debug failed (exit ${status}) but the output does not name the preset"
[ ! -f "$work/preset/CMakeCache.txt" ] || fail "vcpkg-debug left a CMakeCache.txt: the preset was not disabled"

echo "neg-vcpkg-absent: planted run 2 (SL_DEPS_PROVIDERS=vcpkg)"
set +e
out="$(env -u VCPKG_ROOT cmake --preset debug -B "$work/provider" -DSL_DEPS_PROVIDERS=vcpkg 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | grep -E 'sliced: deps|^  - ' | head -n 4
[ "$status" -ne 0 ] || fail "SL_DEPS_PROVIDERS=vcpkg configured without vcpkg (exit 0)"
for pat in 'vcpkg toolchain not active' 'sudo pacman -S vcpkg'; do
  printf '%s\n' "$out" | grep -qF -- "$pat" || fail "provider run failed (exit ${status}) but its output lacks '${pat}'"
done

echo "neg-vcpkg-absent: ok (control exit 0; vcpkg-debug refused; provider vcpkg refused naming 'sudo pacman -S vcpkg')"
