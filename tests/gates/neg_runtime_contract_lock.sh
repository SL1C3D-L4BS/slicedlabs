#!/usr/bin/env bash
# neg-runtime-contract-lock (WO-100, ARCH3-R020 RT-INV-GATE-06): an error code number that drifts between the code and
# SL-ARCH-003 must fail tools/runtime/contract_lock.py naming the code.
#   control: the sl_runtime_dump_contract dump and a copy of docs/arch, the runtime headers and the lock pass (exit 0)
#   (a) the copy of SL-ARCH-003 gives errc stale_entity the number 6059 (the code says 6050): the check exits 1 with
#       R-LOCK-DOC naming errc stale_entity and both numbers
#   (b) the copy of the lock gives errc stale_entity 6059 too: the check fails with R-LOCK-HASH and R-LOCK-LOCK naming
#       errc stale_entity
# Works under build/runtime/neg/contract-lock (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
checker="$root/tools/runtime/contract_lock.py"
dumper="$build/debug/engine/runtime/tools/sl_runtime_dump_contract"
work="$build/runtime/neg/contract-lock"
code="stale_entity"

trap 'rm -rf "$work"' EXIT

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_runtime_dump_contract >/dev/null

fresh() {
  rm -rf "$work"
  mkdir -p "$work/docs" "$work/schemas/runtime" "$work/engine/runtime"
  cp -r "$root/docs/arch" "$work/docs/arch"
  cp -r "$root/engine/runtime/include" "$work/engine/runtime/include"
  cp "$root/schemas/runtime/contract.lock.json" "$work/schemas/runtime/contract.lock.json"
  "$dumper" >"$work/contract.json"
}

fail() {
  echo "neg-runtime-contract-lock: FAIL: $1"
  exit 1
}

# runs the checker on the copy; sets out and status
run_check() {
  set +e
  out="$(python3 -I "$checker" check "$work/contract.json" --root "$work" 2>&1)"
  status=$?
  set -e
}

echo "neg-runtime-contract-lock: control run (runtime dump, repository copy)"
fresh
run_check
[ "$status" -eq 0 ] || {
  echo "$out"
  fail "control run must exit 0 but runtime-contract-lock failed on the unmodified copy"
}

# (a) the document drifts one error code number
fresh
doc="$(find "$work/docs/arch" -name 'SL-ARCH-003-*.md' | head -n 1)"
grep -qF "{\"name\": \"$code\", \"number\": 6050," "$doc" || fail "a-doc-drift: SL-ARCH-003 has no $code 6050 row to drift"
sed -i "s/{\"name\": \"$code\", \"number\": 6050,/{\"name\": \"$code\", \"number\": 6059,/" "$doc"
run_check
[ "$status" -eq 1 ] || fail "a-doc-drift: runtime-contract-lock exited $status with $code drifted to 6059 in SL-ARCH-003"
for want in R-LOCK-DOC "errc $code" 6050 6059; do
  grep -F "errc $code" <<<"$out" | grep -qF -- "$want" || {
    echo "$out"
    fail "a-doc-drift: no error line names errc $code with '$want'"
  }
done
echo "neg-runtime-contract-lock: a-doc-drift -> exit $status, R-LOCK-DOC names errc $code (code 6050, document 6059)"

# (b) the lock drifts the same number (hash left stale)
fresh
lock="$work/schemas/runtime/contract.lock.json"
python3 -I - "$lock" "$code" <<'EOF'
import json, sys
path, code = sys.argv[1], sys.argv[2]
lock = json.load(open(path, encoding="utf-8"))
rows = [r for r in lock["contract"]["error_codes"] if r["name"] == code and r["number"] == 6050]
if len(rows) != 1:
    sys.exit("b-lock-drift: the lock has no %s 6050 row" % code)
rows[0]["number"] = 6059
open(path, "w", encoding="utf-8").write(json.dumps(lock, indent=2) + "\n")
EOF
run_check
[ "$status" -eq 1 ] || fail "b-lock-drift: runtime-contract-lock exited $status with $code drifted in the lock"
grep -qF R-LOCK-HASH <<<"$out" || {
  echo "$out"
  fail "b-lock-drift: no R-LOCK-HASH line"
}
grep -F R-LOCK-LOCK <<<"$out" | grep -qF "errc $code" || {
  echo "$out"
  fail "b-lock-drift: no R-LOCK-LOCK line names errc $code"
}
echo "neg-runtime-contract-lock: b-lock-drift -> exit $status, R-LOCK-HASH and R-LOCK-LOCK name errc $code"

echo "neg-runtime-contract-lock: ok (control exit 0, the document and the lock drifts each failed naming errc $code)"
