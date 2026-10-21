#!/usr/bin/env bash
# neg-runtime-leak (WO-085, SL-ARCH-003 ARCH3-R053, R054, RT-INV-DIAG-06): runtime-memreport-check --no-leaks must
# reject a memory report with a leak and name the leak's tag and site.
#   input: build/runtime/test/memreport.json, written by `just runtime-test diag` (diag_json_test); missing fails.
#   control: the copy as written passes the checker with --no-leaks (exit 0).
#   planted: sed replaces its empty leak list by one leak {"tag": "neg.leak", "site": "neg.cpp:1", "size": 64}; the
#   planted report still passes the schema check without --no-leaks (exit 0) and fails with --no-leaks (exit 1) with
#   output naming neg.leak and neg.cpp:1.
# Works under build/runtime/neg/leak (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/runtime/neg/leak"
source_report="$root/build/runtime/test/memreport.json"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-runtime-leak: FAIL: $1"
  exit 1
}

cd "$root"
[ -f "$source_report" ] || fail "$source_report not found; run \`just runtime-test diag\` first"

rm -rf "$work"
mkdir -p "$work"
cp "$source_report" "$work/memreport.json"

# check <file> [--no-leaks]: sets out and status
check() {
  set +e
  out="$(python3 -I tools/runtime/memreport_check.py "$@" 2>&1)"
  status=$?
  set -e
}

check "$work/memreport.json" --no-leaks
if [ "$status" -ne 0 ]; then
  echo "$out" | head -n 20
  fail "control: the report of runtime-test diag must pass --no-leaks (exit $status)"
fi
echo "neg-runtime-leak: control -> exit 0"

grep -q '"leaks": \[\]' "$work/memreport.json" || fail "the report has no empty leak list to plant into"
sed 's/"leaks": \[\]/"leaks": [{"tag": "neg.leak", "site": "neg.cpp:1", "size": 64}]/' "$work/memreport.json" \
  >"$work/planted.json"
grep -q '"neg.cpp:1"' "$work/planted.json" || fail "the planted leak did not land"

check "$work/planted.json"
if [ "$status" -ne 0 ]; then
  echo "$out" | head -n 20
  fail "planted: the report with a leak must stay schema-valid without --no-leaks (exit $status)"
fi

check "$work/planted.json" --no-leaks
[ "$status" -eq 1 ] || {
  echo "$out" | head -n 20
  fail "planted: --no-leaks exited $status, expected 1"
}
grep -q "neg.leak" <<<"$out" || fail "planted: the output does not name the tag neg.leak"
grep -q "neg.cpp:1" <<<"$out" || fail "planted: the output does not name the site neg.cpp:1"
echo "neg-runtime-leak: planted -> exit 1: $(grep -m1 neg.leak <<<"$out")"
echo "neg-runtime-leak: PASS"
