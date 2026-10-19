#!/usr/bin/env bash
# neg-fabric-conformance (WO-062): an invariant whose test case is deleted must fail fabric-contracts-strict (the last
# step of fabric-conformance) naming the invariant id. Each plant starts from a fresh copy of the checker inputs
# (docs/arch, work-orders, justfile, recipes.json, the fabric C++ test directories and the tools/fabric Python tests).
#   control  the unmodified copy passes `contracts_check.py --strict` (exit 0, pending 0)
#   (a) every FAB-INV-RPL-02 TEST_CASE deleted from replay_pin_test.cpp   -> exit 1 naming FAB-INV-RPL-02
#   (b) every FAB-INV-CF-03 test method deleted from test_contract_lock.py -> exit 1 naming FAB-INV-CF-03
# The throwaway tree is build/neg/fabric-conformance (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/fabric/contracts_check.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-conformance"

trap 'rm -rf "$work"' EXIT

fresh() {
  rm -rf "$work"
  mkdir -p "$work/tools/governance" "$work/tools/fabric" "$work/tests/unit" "$work/tests/integration"
  cp -r "$root/docs" "$work/docs"
  cp -r "$root/work-orders" "$work/work-orders"
  cp "$root/justfile" "$work/justfile"
  cp "$root/tools/governance/recipes.json" "$work/tools/governance/recipes.json"
  cp "$root"/tools/fabric/test_*.py "$work/tools/fabric/"
  for area in unit integration; do
    cp -r "$root/tests/$area/fabric" "$work/tests/$area/fabric"
  done
}

fail() {
  echo "neg-fabric-conformance: FAIL: $1"
  exit 1
}

# delete_cases <file> <start regex>: removes every block from a line matching the regex up to (not including) the
# next line that starts a test case or method, or the end of the file; fails when nothing matched.
delete_cases() {
  python3 -I -c '
import re
import sys
path, start = sys.argv[1:3]
lines = open(path, encoding="utf-8").read().split("\n")
nxt = re.compile(r"^(TEST_CASE|    def test_|if __name__)")
out, skipping, removed = [], False, 0
for line in lines:
    if re.search(start, line):
        skipping, removed = True, removed + 1
        continue
    if skipping and nxt.search(line):
        skipping = False
    if not skipping:
        out.append(line)
if removed == 0:
    sys.exit("no test case matches " + start)
open(path, "w", encoding="utf-8").write("\n".join(out))
print("deleted %d test case(s) of %s" % (removed, path))
' "$1" "$2"
}

# runs the strict checker on the copy; sets out and status
run_strict() {
  set +e
  out="$(python3 -I "$checker" --strict --root "$work" 2>&1)"
  status=$?
  set -e
}

echo "neg-fabric-conformance: control run (unmodified copy)"
fresh
run_strict
[ "$status" -eq 0 ] || {
  echo "$out" | tail -n 8
  fail "control run must exit 0 but fabric-contracts-strict failed on the unmodified copy"
}
grep -q "pending 0" <<<"$out" || fail "control run: pending is not 0"

# (a) a C++ invariant test case deleted
fresh
delete_cases "$work/tests/unit/fabric/replay_pin_test.cpp" '^TEST_CASE\("FAB-INV-RPL-02:'
grep -q "FAB-INV-RPL-02:" "$work/tests/unit/fabric/replay_pin_test.cpp" && fail "a-cpp-case: FAB-INV-RPL-02 still named"
run_strict
[ "$status" -ne 0 ] || fail "a-cpp-case: fabric-contracts-strict exited 0 with the FAB-INV-RPL-02 test cases deleted"
grep -q "FAB-INV-RPL-02" <<<"$out" || {
  echo "$out" | tail -n 8
  fail "a-cpp-case: output does not name FAB-INV-RPL-02"
}
echo "neg-fabric-conformance: a-cpp-case: exit $status, names FAB-INV-RPL-02"

# (b) a Python invariant test method deleted
fresh
delete_cases "$work/tools/fabric/test_contract_lock.py" '^    def test_fab_inv_cf_03_'
run_strict
[ "$status" -ne 0 ] || fail "b-py-case: fabric-contracts-strict exited 0 with the FAB-INV-CF-03 tests deleted"
grep -q "FAB-INV-CF-03" <<<"$out" || {
  echo "$out" | tail -n 8
  fail "b-py-case: output does not name FAB-INV-CF-03"
}
echo "neg-fabric-conformance: b-py-case: exit $status, names FAB-INV-CF-03"
echo "neg-fabric-conformance: ok (control exit 0, 2 deleted invariant tests failed naming their ids)"
