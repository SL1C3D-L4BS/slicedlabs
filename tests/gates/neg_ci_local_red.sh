#!/usr/bin/env bash
# neg-ci-local-red (WO-020): a failing unit test must turn ci-local red and the failing step must be named.
# Works on a throwaway COPY of the working tree under build/neg/ci-red (no commit needed):
#   1. copy the tree (tracked and untracked, ignored files left out), git init there
#   2. control run: `bash scripts/ci-local.sh --quick` must exit 0
#   3. plant TEST_CASE("planted") { CHECK(1 == 2); } in tests/unit/smoke.cpp, run again (same build tree):
#      must exit non-zero, name the step `workflow ci-clang` and its test phase
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/ci-red"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

git ls-files -z --cached --others --exclude-standard | tar --null --files-from=- -cf - | tar -xf - -C "$work"

cd "$work"
git init -q
git add -A

echo "neg-ci-local-red: control run (clean copy, ci-local.sh --quick)"
if ! out="$(bash scripts/ci-local.sh --quick 2>&1)"; then
  printf '%s\n' "$out" | tail -n 30
  echo "neg-ci-local-red: FAIL: control run must exit 0 but ci-local failed on the clean copy"
  exit 1
fi

printf '\nTEST_CASE("planted") { CHECK(1 == 2); }\n' >>tests/unit/smoke.cpp
git add -A

echo "neg-ci-local-red: planted run (failing TEST_CASE in tests/unit/smoke.cpp)"
set +e
out="$(bash scripts/ci-local.sh --quick 2>&1)"
status=$?
set -e
printf '%s\n' "$out" | grep -E "FAILED at step|planted" | head -n 4

if [ "$status" -eq 0 ]; then
  echo "neg-ci-local-red: FAIL: ci-local exited 0 with a failing test"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q "FAILED at step 'workflow ci-clang' (Executing workflow step 3 of 3: test preset"; then
  echo "neg-ci-local-red: FAIL: ci-local failed (exit $status) without naming the test step of workflow ci-clang"
  exit 1
fi
echo "neg-ci-local-red: ok (control exit 0, planted exit $status at the test step of workflow ci-clang)"
