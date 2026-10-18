#!/usr/bin/env bash
# neg-golden (WO-027): a changed golden file must fail its test and print a diff.
# Works on a throwaway COPY of the working tree under build/neg/golden (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), configure and build the debug preset, then
#      `ctest -R golden` on the clean copy (control): must exit 0
#   2. change one line of tests/golden/build_info.golden in the copy (planted): ctest must exit non-zero and its output
#      must hold the diff markers (--- / +++ / @@ and the changed lines)
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/golden"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git --exclude /.claude "$root"/ "$work"/
cd "$work"

echo "neg-golden: control run (clean copy)"
if ! { cmake --preset debug && cmake --build --preset debug --target golden_smoke; } >control.log 2>&1; then
  tail -n 15 control.log
  echo "neg-golden: FAIL: the clean copy did not configure and build"
  exit 1
fi
if ! ctest --test-dir build/debug -R golden --no-tests=error --output-on-failure >control-ctest.log 2>&1; then
  tail -n 15 control-ctest.log
  echo "neg-golden: FAIL: control ctest must exit 0 but failed"
  exit 1
fi

golden="tests/golden/build_info.golden"
sed -i 's/^  linker     .*/  linker     PLANTED-LINKER/' "$golden"
if cmp -s "$golden" "$root/$golden"; then
  echo "neg-golden: FAIL: the planted edit did not change $golden"
  exit 1
fi
echo "neg-golden: planted run (edited the linker line of $golden)"
set +e
ctest --test-dir build/debug -R golden --no-tests=error --output-on-failure >planted.log 2>&1
status=$?
set -e
grep -E '^(---|\+\+\+|@@|[-+] )' planted.log | head -n 8

if [ "$status" -eq 0 ]; then
  echo "neg-golden: FAIL: ctest exited 0 with a changed golden file"
  exit 1
fi
for marker in '^--- golden/build_info.golden' '^\+\+\+ actual' '^@@ ' '^-  linker     PLANTED-LINKER' '^\+  linker     <linker>'; do
  if ! grep -Eq "$marker" planted.log; then
    echo "neg-golden: FAIL: ctest exited $status but its output has no diff line matching: $marker"
    exit 1
  fi
done
echo "neg-golden: ok (control exit 0, planted exit $status, diff printed)"
