#!/usr/bin/env bash
# neg-tidy (WO-025): a bugprone defect planted in a copy of the tree must make `just tidy` fail and name the check.
# Works on a throwaway COPY of the working tree under build/neg/tidy (no commit needed):
#   1. copy the tree (rsync, build/ and .git left out), control run of `just tidy`: must exit 0
#   2. add examples/sandbox/planted_tidy.cpp (a use after move, bugprone-use-after-move) to sl_sandbox in the copy's
#      CMake, run `just tidy` again: must exit non-zero and print the check name and the planted file
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/tidy"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
rsync -a --exclude /build --exclude /.git "$root"/ "$work"/

cd "$work"
echo "neg-tidy: control run (clean copy)"
if ! just tidy >control.log 2>&1; then
  tail -n 25 control.log
  echo "neg-tidy: FAIL: control run must exit 0 but tidy failed on the clean copy"
  exit 1
fi

cat >examples/sandbox/planted_tidy.cpp <<'CPP'
// planted by tests/gates/neg_tidy.sh: reads a std::string after it was moved from
#include <string>
#include <utility>

int planted_use_after_move() {
  std::string original{"planted"};
  const std::string taken{std::move(original)};
  return static_cast<int>(original.size() + taken.size());
}
CPP
printf '\ntarget_sources(sl_sandbox PRIVATE planted_tidy.cpp)\n' >>examples/sandbox/CMakeLists.txt

echo "neg-tidy: planted run (examples/sandbox/planted_tidy.cpp: use after move)"
set +e
just tidy >planted.log 2>&1
status=$?
set -e
grep -E 'planted_tidy.cpp:[0-9]+:[0-9]+: error' planted.log | head -n 3 || true

if [ "$status" -eq 0 ]; then
  echo "neg-tidy: FAIL: tidy exited 0 with a use after move planted"
  exit 1
fi
if ! grep -qE 'planted_tidy\.cpp:[0-9]+:[0-9]+: error: .*\[bugprone-use-after-move' planted.log; then
  tail -n 25 planted.log
  echo "neg-tidy: FAIL: tidy failed (exit $status) without naming bugprone-use-after-move in planted_tidy.cpp"
  exit 1
fi
echo "neg-tidy: ok (control exit 0, planted exit $status, bugprone-use-after-move named in planted_tidy.cpp)"
