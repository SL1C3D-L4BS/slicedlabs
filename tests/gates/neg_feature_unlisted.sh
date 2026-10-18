#!/usr/bin/env bash
# neg-feature-unlisted (WO-015): a C++23 feature test in no list must stop configure and be named.
# Works on a throwaway COPY of the working tree under build/neg/feature_unlisted (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), control configure of the debug preset: must exit 0
#   2. add cmake/features/cxx23/zz_unlisted.cpp (in neither required-cxx23.txt nor deferred-cxx23.txt) and
#      configure again in the same build tree: must exit non-zero and name zz_unlisted.cpp
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/feature_unlisted"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-feature-unlisted: control run (clean copy)"
if ! cmake --preset debug >control.log 2>&1; then
  tail -n 15 control.log
  echo "neg-feature-unlisted: FAIL: control configure must exit 0 but failed on the clean copy"
  exit 1
fi

printf '// planted by neg-feature-unlisted\nint main() { return 0; }\n' >cmake/features/cxx23/zz_unlisted.cpp
echo "neg-feature-unlisted: planted run (added cmake/features/cxx23/zz_unlisted.cpp)"
set +e
cmake --preset debug >planted.log 2>&1
status=$?
set -e
grep -A4 'CMake Error' planted.log | head -n 8

if [ "$status" -eq 0 ]; then
  echo "neg-feature-unlisted: FAIL: configure exited 0 with an unlisted feature test"
  exit 1
fi
if ! grep -q 'zz_unlisted' planted.log; then
  echo "neg-feature-unlisted: FAIL: configure failed (exit $status) but its output does not name zz_unlisted"
  exit 1
fi
echo "neg-feature-unlisted: ok (control exit 0, planted exit $status, zz_unlisted named)"
