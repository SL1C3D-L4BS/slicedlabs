#!/usr/bin/env bash
# neg-runtime-contracts (WO-077, ARCH3-R014): planted contract drift must make the runtime contracts checker fail and
# name the rule and the id. Each plant starts from a fresh copy of the checker inputs (docs/arch, work-orders, justfile,
# recipes.json, scripts/runtime, the runtime C++ test directories and the tools/runtime Python tests).
#   control  the unmodified copy exits 0 (pending tolerated); no R-RT-MISSING-TEST line names RT-INV-LINT-03
#   (1) the test_rt_inv_lint_03_ cases renamed away   -> runtime-contracts-strict exits 1 with R-RT-MISSING-TEST RT-INV-LINT-03
#                                                        (and the default mode lists it as pending, exit 0)
#   (2) a TEST_CASE("RT-INV-ZZ-99: ...") file         -> R-RT-ORPHAN-TEST RT-INV-ZZ-99
#   (3) a second file naming RT-INV-LINT-01           -> R-RT-LOCATION RT-INV-LINT-01
# The throwaway tree is build/runtime/neg/contracts (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/contracts"
checker="$root/tools/runtime/contracts_check.py"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-contracts: FAIL: $1"
  exit 1
}

fresh() {
  rm -rf "$work"
  mkdir -p "$work/tools/governance" "$work/tools/runtime" "$work/tests/unit" "$work/tests/integration" "$work/scripts"
  cp -r "$root/docs" "$work/docs"
  cp -r "$root/work-orders" "$work/work-orders"
  cp "$root/justfile" "$work/justfile"
  cp "$root/tools/governance/recipes.json" "$work/tools/governance/recipes.json"
  cp -r "$root/scripts/runtime" "$work/scripts/runtime"
  cp "$root"/tools/runtime/test_*.py "$work/tools/runtime/"
  for area in unit integration; do
    if [ -d "$root/tests/$area/runtime" ]; then cp -r "$root/tests/$area/runtime" "$work/tests/$area/runtime"; fi
  done
  mkdir -p "$work/tests/unit/runtime"
}

# run_checker [flags]: sets out and status
run_checker() {
  set +e
  out="$(python3 -I "$checker" --root "$work" "$@" 2>&1)"
  status=$?
  set -e
}

# expect_line <name> <rule> <id>: out holds one line naming both the rule and the id (a pending line does not count)
named_lines() {
  grep -v '^pending ' <<<"$out" | grep -F "$1" | grep -F "$2" || true
}

expect_line() {
  if [ -z "$(named_lines "$2" "$3")" ]; then
    echo "$out" | head -n 6
    fail "$1: no error line names both $2 and $3"
  fi
}

echo "neg-runtime-contracts: control run (unmodified copy)"
fresh
run_checker
[ "$status" -eq 0 ] || {
  echo "$out" | head -n 8
  fail "control: the checker must exit 0 on the unmodified copy"
}
run_checker --strict
[ -z "$(named_lines R-RT-MISSING-TEST RT-INV-LINT-03)" ] || fail "control: RT-INV-LINT-03 must have its test before the plant"

fresh
sed -i 's/def test_rt_inv_lint_03_/def helper_renamed_lint_03_/' "$work/tools/runtime/test_contracts_check.py"
grep -q 'def helper_renamed_lint_03_' "$work/tools/runtime/test_contracts_check.py" || fail "1-missing-test: the plant did not apply"
run_checker --strict
[ "$status" -eq 1 ] || fail "1-missing-test: --strict exited $status, expected 1"
expect_line "1-missing-test" R-RT-MISSING-TEST RT-INV-LINT-03
echo "neg-runtime-contracts: 1-missing-test -> --strict exit $status, R-RT-MISSING-TEST RT-INV-LINT-03 named"
run_checker
[ "$status" -eq 0 ] || fail "1-missing-test: the default mode must tolerate a pending test (exit $status)"
grep -qF RT-INV-LINT-03 <<<"$out" || fail "1-missing-test: the default mode does not list RT-INV-LINT-03 as pending"
echo "neg-runtime-contracts: 1-missing-test -> default mode exit 0, RT-INV-LINT-03 listed as pending"

fresh
printf 'TEST_CASE("RT-INV-ZZ-99: planted orphan") {}\n' >"$work/tests/unit/runtime/zz_orphan_test.cpp"
run_checker
[ "$status" -eq 1 ] || fail "2-orphan-test: exited $status, expected 1"
expect_line "2-orphan-test" R-RT-ORPHAN-TEST RT-INV-ZZ-99
echo "neg-runtime-contracts: 2-orphan-test -> exit $status, R-RT-ORPHAN-TEST RT-INV-ZZ-99 named"

fresh
printf 'TEST_CASE("RT-INV-LINT-01: planted in the wrong file") {}\n' >"$work/tests/unit/runtime/zz_location_test.cpp"
run_checker
[ "$status" -eq 1 ] || fail "3-location: exited $status, expected 1"
expect_line "3-location" R-RT-LOCATION RT-INV-LINT-01
echo "neg-runtime-contracts: 3-location -> exit $status, R-RT-LOCATION RT-INV-LINT-01 named"

echo "neg-runtime-contracts: ok (control exit 0, 3 planted defects each failed naming the rule and the id)"
