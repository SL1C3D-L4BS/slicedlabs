#!/usr/bin/env bash
# neg-test-timeout (WO-026): a test that outlives its TIMEOUT must fail ctest and be reported as a timeout.
# Works on a throwaway COPY of the working tree under build/neg/test_timeout (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), add a unit test `neg_sleeper` with TIMEOUT 1 that sleeps 0.1 s
#      (control): configure, build and `ctest -R neg_sleeper` must exit 0
#   2. make it sleep 3 s (planted), rebuild: ctest must exit non-zero and print "Timeout"
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/test_timeout"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git --exclude /.claude "$root"/ "$work"/
cd "$work"

write_sleeper() {
  cat >tests/unit/neg_sleeper.cpp <<SRC
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

TEST_CASE("sleeps") {
  std::this_thread::sleep_for(std::chrono::milliseconds($1));
  CHECK(true);
}
SRC
}
printf '\nsl_add_test(NAME neg_sleeper TIER unit SOURCES neg_sleeper.cpp TIMEOUT 1)\n' >>tests/unit/CMakeLists.txt

echo "neg-test-timeout: control run (sleeps 100 ms, TIMEOUT 1)"
write_sleeper 100
if ! { cmake --preset debug && cmake --build --preset debug --target neg_sleeper; } >control.log 2>&1; then
  tail -n 15 control.log
  echo "neg-test-timeout: FAIL: the clean copy did not configure and build"
  exit 1
fi
if ! ctest --test-dir build/debug -R '^neg_sleeper$' --no-tests=error >control-ctest.log 2>&1; then
  tail -n 15 control-ctest.log
  echo "neg-test-timeout: FAIL: control ctest must exit 0 but failed"
  exit 1
fi

echo "neg-test-timeout: planted run (sleeps 3 s, TIMEOUT 1)"
write_sleeper 3000
cmake --build --preset debug --target neg_sleeper >planted-build.log 2>&1
set +e
ctest --test-dir build/debug -R '^neg_sleeper$' --no-tests=error >planted.log 2>&1
status=$?
set -e
grep -i 'timeout' planted.log | head -n 4

if [ "$status" -eq 0 ]; then
  echo "neg-test-timeout: FAIL: ctest exited 0 although the test outlived its TIMEOUT"
  exit 1
fi
if ! grep -q 'Timeout' planted.log; then
  echo "neg-test-timeout: FAIL: ctest exited $status but did not report Timeout"
  exit 1
fi
echo "neg-test-timeout: ok (control exit 0, planted exit $status, ctest reported Timeout)"
