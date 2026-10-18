#!/usr/bin/env bash
# neg-fabric-crash-sweep (WO-055): the per-primitive crash sweep (FAB-INV-RCV-09) must catch a log
# backend that acknowledges a commit before its frames are durable.
#   control: the crash sweep of the real tree (build/debug) passes at a reduced scale
#   planted: a copy of the tree whose log_backend::commit skips the segment sync after its append
#            (the head is published and the commit acknowledged while its frames are not durable;
#            the script asserts the edit happened) builds fabric_recovery_crash_sweep_test, whose
#            primitive sweep must fail naming FAB-INV-RCV-09 and an acknowledged commit lost
# Works under build/neg/fabric-crash-sweep (SL_BUILD_DIR overrides build); the repository is never
# modified. The copy shares the repository's ccache directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/neg/fabric-crash-sweep"
copy="$work/src"
backend="engine/fabric/src/logstore_backend.cpp"
needle='sync_file(seg.file, s.options.sync)'
test_name="fabric_recovery_crash_sweep_test"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-fabric-crash-sweep: FAIL: $1"
  exit 1
}

echo "neg-fabric-crash-sweep: control run (the real tree's crash sweep)"
cmake --preset debug >/dev/null
cmake --build --preset debug --target "$test_name" >/dev/null
if ! SL_FABRIC_SWEEP_SCALE=0.3 ctest --preset debug -R "^${test_name}\$" --no-tests=error >/dev/null; then
  fail "control run must pass but the crash sweep failed on the unmodified tree"
fi

echo "neg-fabric-crash-sweep: planted copy (commit acknowledged before its frames are durable)"
rm -rf "$work"
mkdir -p "$copy"
rsync -a --exclude=/build --exclude=/.git "$root/" "$copy/"
[ "$(grep -cF "$needle" "$copy/$backend")" -eq 1 ] ||
  fail "the edit point '$needle' is not exactly once in $backend"
sed -i "s/sync_file(seg\.file, s\.options\.sync)/result<void>{}/" "$copy/$backend"
if grep -qF "$needle" "$copy/$backend" || ! grep -qF 'result<void> ok = result<void>{}' "$copy/$backend"; then
  fail "the planted edit did not apply to $backend"
fi
cmake -S "$copy" -B "$copy/build/debug" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DSL_PRESET=debug -DSL_CCACHE_DIR="$build/.ccache" >/dev/null
cmake --build "$copy/build/debug" --target "$test_name" >/dev/null
exe="$copy/build/debug/tests/integration/fabric/$test_name"
[ -x "$exe" ] || fail "the planted copy did not build $test_name"
set +e
out="$(cd "$copy/build/debug/tests/integration/fabric" &&
  SL_FABRIC_SWEEP_SCALE=0.3 "$exe" --test-case='*storage primitive*' 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "planted: the crash sweep passed although acknowledged commits are not durable"
for want in "FAB-INV-RCV-09" "acknowledged commit lost"; do
  grep -qF "$want" <<<"$out" || {
    echo "$out" | head -n 20
    fail "planted: the failing sweep does not name '$want'"
  }
done
echo "neg-fabric-crash-sweep: planted: exit $status, names FAB-INV-RCV-09 and a lost acknowledged commit"
echo "neg-fabric-crash-sweep: ok (control passed, the planted durability bug failed the sweep)"
