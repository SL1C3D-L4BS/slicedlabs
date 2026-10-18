#!/usr/bin/env bash
# neg-probe-never-fails (WO-015): a C++26 probe that cannot compile is reported, never fatal.
# Works on a throwaway COPY of the working tree under build/neg/probe_never_fails (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), control configure of the debug preset: must exit 0
#   2. add cmake/features/cxx26/zz_fail.cpp (an #error) to probes-cxx26.txt and configure again in the same
#      build tree: must exit 0 and build/debug/features/cxx26-report.json must mark zz_fail as fail
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/probe_never_fails"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-probe-never-fails: control run (clean copy)"
if ! cmake --preset debug >control.log 2>&1; then
  tail -n 15 control.log
  echo "neg-probe-never-fails: FAIL: control configure must exit 0 but failed on the clean copy"
  exit 1
fi

printf '#error "planted by neg-probe-never-fails"\nint main() { return 0; }\n' >cmake/features/cxx26/zz_fail.cpp
printf 'zz_fail\n' >>cmake/features/probes-cxx26.txt
echo "neg-probe-never-fails: planted run (added the failing probe zz_fail)"
set +e
cmake --preset debug >planted.log 2>&1
status=$?
set -e

if [ "$status" -ne 0 ]; then
  grep -A4 'CMake Error' planted.log | head -n 8
  echo "neg-probe-never-fails: FAIL: configure exited $status because of a C++26 probe"
  exit 1
fi
if ! python3 -I -c 'import json, sys; r = json.load(open(sys.argv[1])); assert r["probes"]["zz_fail"]["status"] == "fail", r["probes"]["zz_fail"]' \
  build/debug/features/cxx26-report.json; then
  echo "neg-probe-never-fails: FAIL: build/debug/features/cxx26-report.json does not mark zz_fail as fail"
  exit 1
fi
echo "neg-probe-never-fails: ok (control exit 0, planted exit 0, zz_fail marked fail in the report)"
