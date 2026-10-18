#!/usr/bin/env bash
# neg-feature-removed (WO-015): deleting a required C++23 feature test must stop configure with the exact message.
# Works on a throwaway COPY of the working tree under build/neg/feature_removed (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), control configure of the debug preset: must exit 0
#   2. delete cmake/features/cxx23/if_consteval.cpp (the id stays in required-cxx23.txt), configure again in the
#      same build tree (a warm cache must not hide it): must exit non-zero and print the exact message
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/feature_removed"
id="if_consteval"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-feature-removed: control run (clean copy)"
if ! cmake --preset debug >control.log 2>&1; then
  tail -n 15 control.log
  echo "neg-feature-removed: FAIL: control configure must exit 0 but failed on the clean copy"
  exit 1
fi

rm "cmake/features/cxx23/${id}.cpp"
echo "neg-feature-removed: planted run (deleted cmake/features/cxx23/${id}.cpp)"
set +e
cmake --preset debug >planted.log 2>&1
status=$?
set -e
grep -A4 'CMake Error' planted.log | head -n 8

expected="sliced: required C++23 feature test '${id}' is listed in cmake/features/required-cxx23.txt but cmake/features/cxx23/${id}.cpp does not exist. A deleted test hides a feature the code relies on. Restore the file, or remove the id through an ADR (SL-GOV-004) and a change to SL-EP-043."
flat="$(tr -s ' \n' '  ' <planted.log)"
if [ "$status" -eq 0 ]; then
  echo "neg-feature-removed: FAIL: configure exited 0 with a required test file deleted"
  exit 1
fi
if [[ "$flat" != *"$expected"* ]]; then
  echo "neg-feature-removed: FAIL: configure failed (exit $status) without the exact message:"
  echo "  $expected"
  exit 1
fi
echo "neg-feature-removed: ok (control exit 0, planted exit $status, exact message printed)"
