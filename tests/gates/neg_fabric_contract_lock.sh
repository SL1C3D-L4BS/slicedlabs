#!/usr/bin/env bash
# neg-fabric-contract-lock (WO-062): a contract number that drifts between the code and a document must fail
# tools/fabric/contract_lock.py naming the error code.
#   control: the engine's contract dump and a copy of both ARCH documents and the lock pass (exit 0)
#   (a) the copy of SL-ARCH-001 gives errc store_io the number 5016 (the code says 5013): the check must exit
#       non-zero with R-LOCK-DOC naming errc store_io and both numbers
#   (b) the copy of the lock gives errc store_io 5016 too: the check must fail with R-LOCK-HASH and R-LOCK-LOCK
#       naming errc store_io
# Works under build/neg/fabric-contract-lock (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
checker="$root/tools/fabric/contract_lock.py"
dumper="$build/debug/engine/fabric/tools/sl_fabric_dump_contract"
work="$build/neg/fabric-contract-lock"
code="store_io"

trap 'rm -rf "$work"' EXIT

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_fabric_dump_contract >/dev/null

fresh() {
  rm -rf "$work"
  mkdir -p "$work/docs" "$work/schemas/fabric"
  cp -r "$root/docs/arch" "$work/docs/arch"
  cp "$root/schemas/fabric/contract.lock.json" "$work/schemas/fabric/contract.lock.json"
  "$dumper" "$work/contract.json"
}

fail() {
  echo "neg-fabric-contract-lock: FAIL: $1"
  exit 1
}

# runs the checker on the copy; sets out and status
run_check() {
  set +e
  out="$(python3 -I "$checker" check "$work/contract.json" --root "$work" 2>&1)"
  status=$?
  set -e
}

echo "neg-fabric-contract-lock: control run (engine dump, repository copy)"
fresh
run_check
[ "$status" -eq 0 ] || {
  echo "$out"
  fail "control run must exit 0 but fabric-contract-lock failed on the unmodified copy"
}

# (a) a document drifts one error code number
fresh
doc="$(find "$work/docs/arch" -name 'SL-ARCH-001-*.md' | head -n 1)"
grep -qF "[\"$code\", 5013]" "$doc" || fail "a-doc-drift: SL-ARCH-001 has no [\"$code\", 5013] row to drift"
sed -i "s/\[\"$code\", 5013\]/[\"$code\", 5016]/" "$doc"
run_check
[ "$status" -ne 0 ] || fail "a-doc-drift: fabric-contract-lock exited 0 with $code drifted to 5016 in SL-ARCH-001"
for want in R-LOCK-DOC "errc $code" 5013 5016; do
  grep -qF "$want" <<<"$out" || {
    echo "$out" | head -n 8
    fail "a-doc-drift: output does not name '$want'"
  }
done
echo "neg-fabric-contract-lock: a-doc-drift: exit $status, names R-LOCK-DOC and errc $code (code 5013, document 5016)"

# (b) the lock drifts the same number
fresh
sed -i "s/\"$code\",[[:space:]]*5013/\"$code\", 5016/" "$work/schemas/fabric/contract.lock.json"
grep -qF "\"$code\", 5016" "$work/schemas/fabric/contract.lock.json" || fail "b-lock-drift: the lock copy was not changed"
run_check
[ "$status" -ne 0 ] || fail "b-lock-drift: fabric-contract-lock exited 0 with $code drifted in the lock"
for want in R-LOCK-HASH R-LOCK-LOCK "errc $code"; do
  grep -qF "$want" <<<"$out" || {
    echo "$out" | head -n 8
    fail "b-lock-drift: output does not name '$want'"
  }
done
echo "neg-fabric-contract-lock: b-lock-drift: exit $status, names R-LOCK-HASH, R-LOCK-LOCK and errc $code"
echo "neg-fabric-contract-lock: ok (control exit 0, 2 planted drifts failed naming errc $code)"
