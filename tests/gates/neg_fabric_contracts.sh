#!/usr/bin/env bash
# neg-fabric-contracts (WO-036): planted contract drift must make the fabric contracts checker fail naming the rule
# and the id. Each plant starts from a fresh copy of the checker inputs (docs/arch, work-orders, justfile,
# recipes.json, the fabric C++ test directories and the tools/fabric Python tests).
#   control  the unmodified copy exits 0
#   (1) a TEST_CASE("FAB-INV-ZZ-99: ...") file                       -> R-FAB-ORPHAN-TEST FAB-INV-ZZ-99
#   (2) a second entry that reuses FAB-INV-ID-01                     -> R-FAB-DUP FAB-INV-ID-01
#   (3) FAB-INV-ID-02 owned by WO-999, which has no work order file  -> R-FAB-WO FAB-INV-ID-02
# The throwaway tree is build/neg/fabric-contracts (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/fabric/contracts_check.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-contracts"
doc="docs/arch/SL-ARCH-001-canonical-project-model.md"

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
    if [ -d "$root/tests/$area/fabric" ]; then cp -r "$root/tests/$area/fabric" "$work/tests/$area/fabric"; fi
  done
  mkdir -p "$work/tests/unit/fabric"
}

# edit_doc <python expression on text `t`, returning the new text>: exactly one change to the copied ARCH-001
edit_doc() {
  python3 -I -c '
import sys
path, expr = sys.argv[1:3]
t = open(path, encoding="utf-8").read()
new = eval(expr)
if new == t:
    sys.exit("neg-fabric-contracts: FAIL: plant changed nothing in " + path)
open(path, "w", encoding="utf-8").write(new)
' "$work/$doc" "$1"
}

# expect_fail <name> <rule> <id>: the checker must exit non-zero with one line naming both
expect_fail() {
  local name="$1" rule="$2" fid="$3" out status
  set +e
  out="$(python3 -I "$checker" --root "$work" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-fabric-contracts: FAIL: $name exited 0, expected non-zero with $rule for $fid"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep "$rule" | grep -q "$fid"; then
    echo "neg-fabric-contracts: FAIL: $name failed (exit $status) but no line names both $rule and $fid"
    printf '%s\n' "$out" | head -n 4
    exit 1
  fi
  echo "neg-fabric-contracts: $name -> exit $status, $rule $fid named"
}

echo "neg-fabric-contracts: control run (unmodified copy)"
fresh
if ! python3 -I "$checker" --root "$work" >/dev/null; then
  echo "neg-fabric-contracts: FAIL: control run must exit 0 but the checker failed on the unmodified copy"
  exit 1
fi

fresh
printf 'TEST_CASE("FAB-INV-ZZ-99: planted orphan") {}\n' >"$work/tests/unit/fabric/zz_orphan_test.cpp"
expect_fail "1-orphan-test" R-FAB-ORPHAN-TEST FAB-INV-ZZ-99

fresh
edit_doc 't.replace("{\"id\": \"FAB-INV-ID-01\"", "{\"id\": \"FAB-INV-ID-01\", \"rule\": \"ARCH1-R01\", \"text\": \"planted twin\", \"test\": \"tests/unit/fabric/id_text_test.cpp\", \"wo\": \"WO-037\", \"verified_by\": \"just fabric-test id\"},\n{\"id\": \"FAB-INV-ID-01\"", 1)'
expect_fail "2-duplicate-id" R-FAB-DUP FAB-INV-ID-01

fresh
edit_doc '__import__("re").sub(r"(\{\"id\": \"FAB-INV-ID-02\".*?\"wo\": \")WO-[0-9]+", r"\g<1>WO-999", t, count=1)'
expect_fail "3-missing-work-order" R-FAB-WO FAB-INV-ID-02

echo "neg-fabric-contracts: ok (control exit 0, 3 planted defects each failed naming the rule and the id)"
