#!/usr/bin/env bash
# neg-fabric-fsck (WO-059, FAB-INV-COR-06): sl_fabric_fsck must catch one flipped byte in an
# object frame and name the object.
#   control: a fresh sample store (sl_fabric_mkstore) checks clean (exit 0)
#   planted: a second fresh store with one byte flipped in the middle of the payload of an object
#            frame in the middle of the log (the object listed by `sl_fabric_fsck --objects`; the
#            script asserts the byte changed) must make sl_fabric_fsck exit 1 and print that
#            object id
# Works under build/neg/fabric-fsck (SL_BUILD_DIR overrides build); the repository is never
# modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/neg/fabric-fsck"
tools="$root/build/debug/engine/fabric/tools"

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-fabric-fsck: FAIL: $1"
  exit 1
}

cd "$root"
cmake --preset debug >/dev/null
cmake --build --preset debug --target sl_fabric_mkstore sl_fabric_fsck >/dev/null
rm -rf "$work"
mkdir -p "$work"

echo "neg-fabric-fsck: control run (a fresh sample store)"
"$tools/sl_fabric_mkstore" "$work/control" >/dev/null
if ! "$tools/sl_fabric_fsck" "$work/control" >"$work/control.out" 2>&1; then
  cat "$work/control.out"
  fail "control run must exit 0 but sl_fabric_fsck reported the fresh store"
fi

echo "neg-fabric-fsck: planted store (one byte flipped inside an object frame)"
"$tools/sl_fabric_mkstore" "$work/planted" >/dev/null
"$tools/sl_fabric_fsck" --objects "$work/planted" >"$work/objects.out"
mapfile -t objects < <(grep '^object [0-9a-f]\{64\} log/' "$work/objects.out")
[ "${#objects[@]}" -ge 3 ] || fail "the sample store lists ${#objects[@]} log objects, expected at least 3"
# object <oid> <file> frame <offset> payload <offset> length <n>
read -r _ oid file _ _ _ payload _ length <<<"${objects[$((${#objects[@]} / 2))]}"
target="$work/planted/.slfabric/$file"
[ -f "$target" ] || fail "the object file $target does not exist"
offset=$((payload + length / 2))
before="$(od -An -tu1 -j "$offset" -N1 "$target" | tr -d ' ')"
printf '%b' "\\0$(printf '%03o' $((before ^ 0x5a)))" |
  dd of="$target" bs=1 seek="$offset" count=1 conv=notrunc status=none
after="$(od -An -tu1 -j "$offset" -N1 "$target" | tr -d ' ')"
[ "$before" != "$after" ] || fail "the planted flip did not change byte $offset of $file"

set +e
out="$("$tools/sl_fabric_fsck" "$work/planted" 2>&1)"
status=$?
set -e
[ "$status" -eq 1 ] || {
  echo "$out" | head -n 20
  fail "planted: sl_fabric_fsck exited $status, expected 1"
}
grep -qF "$oid" <<<"$out" || {
  echo "$out" | head -n 20
  fail "planted: the findings do not name the damaged object $oid"
}
echo "neg-fabric-fsck: planted: byte $offset of $file flipped, exit 1, names object $oid"
echo "neg-fabric-fsck: ok (control clean, the flipped object frame was found and named)"
