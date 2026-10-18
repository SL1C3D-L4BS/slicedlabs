#!/usr/bin/env bash
# neg-fabric-f2 (WO-048): a second ownership claim must be refused and named with both owners, at both ends of the F2
# enforcement point.
#   control: the engine's registry dump and a copy of the repository files the checker reads pass
#            tools/fabric/ownership_check.py (exit 0, numerator 0)
#   (a) the C++ registry: sl_fabric_dump_domains --claim fabric.history=05 must exit non-zero, report
#       ownership_conflict and name owners 02 and 05; ownership_check.py on that dump must fail with R-F2-REJECTED
#   (b) the schema files: a module zz.dup claiming fabric.history with owner 05 added to the copy must fail
#       ownership_check.py with R-F2-DUP naming fabric.history, owner 02 and owner 05
# Works under build/neg/fabric-f2 (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
checker="$root/tools/fabric/ownership_check.py"
dumper="$build/debug/engine/fabric/tools/sl_fabric_dump_domains"
work="$build/neg/fabric-f2"
domain="fabric.history"

trap 'rm -rf "$work"' EXIT

cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_fabric_dump_domains >/dev/null

# a copy of the repository files the checker reads
fresh() {
  rm -rf "$work"
  mkdir -p "$work/docs" "$work/schemas"
  cp -r "$root/docs/arch" "$work/docs/arch"
  cp -r "$root/schemas/fabric" "$work/schemas/fabric"
  "$dumper" "$work/domains.json"
}

fail() {
  echo "neg-fabric-f2: FAIL: $1"
  exit 1
}

echo "neg-fabric-f2: control run (engine dump, repository copy)"
fresh
if ! python3 -I "$checker" "$work/domains.json" --root "$work"; then
  fail "control run must exit 0 but fabric-ownership-check failed on the unmodified copy"
fi

# (a) the C++ registry refuses the second claim and names both owners
fresh
set +e
err="$("$dumper" --claim "$domain=05" "$work/planted.json" 2>&1 >/dev/null)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "a-cpp-second-claim: sl_fabric_dump_domains exited 0 on a second claim of $domain"
for want in ownership_conflict "$domain" "02 (fabric)" "05 (content)"; do
  grep -qF "$want" <<<"$err" || {
    echo "$err"
    fail "a-cpp-second-claim: stderr does not name '$want'"
  }
done
set +e
out="$(python3 -I "$checker" "$work/planted.json" --root "$work" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "a-cpp-second-claim: fabric-ownership-check exited 0 on a dump with a refused claim"
grep -q "R-F2-REJECTED" <<<"$out" || {
  echo "$out" | head -n 8
  fail "a-cpp-second-claim: no R-F2-REJECTED"
}
echo "neg-fabric-f2: a-cpp-second-claim: exit $status, refused with ownership_conflict naming 02 and 05"

# (b) a schema module claims the domain with another owner
fresh
cat >"$work/schemas/fabric/zz.dup.sltype.json" <<EOF
{"format": "sl-type-def/1", "module": "zz.dup", "domain": "$domain", "owner": "05", "types": []}
EOF
set +e
out="$(python3 -I "$checker" "$work/domains.json" --root "$work" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "b-schema-second-owner: fabric-ownership-check exited 0, expected R-F2-DUP"
for want in R-F2-DUP "$domain" "owner 02" "owner 05" "f2 numerator 1"; do
  grep -qF "$want" <<<"$out" || {
    echo "$out" | head -n 8
    fail "b-schema-second-owner: output does not name '$want'"
  }
done
echo "neg-fabric-f2: b-schema-second-owner: exit $status, names R-F2-DUP, $domain, owners 02 and 05"
echo "neg-fabric-f2: ok (control exit 0, 2 planted second claims failed naming both owners)"
