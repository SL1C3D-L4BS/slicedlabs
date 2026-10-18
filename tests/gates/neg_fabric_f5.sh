#!/usr/bin/env bash
# neg-fabric-f5 (WO-051, ADR-0016): both F5 detectors must fail on a planted bypass and name it.
#   control: fabric-surface-check passes on a copy of the public headers, and the F5 audit (fabric_f5_ledger_test,
#            preset debug) passes with numerator 0
#   (1) a public `value &at_mut();` planted in a copy of value.hpp fails fabric-surface-check naming R-F5-SETTER,
#       value::at_mut and the planted file:line
#   (2) the f5::authorized_scope line removed from a copy of engine/fabric/src/tx_manager.cpp (the script checks the
#       edit applied): the copy's fabric_f5_ledger_test, built with the debug preset, fails and its F5 audit reports
#       bypassing > 0 and mutations > 0
# Works under build/neg/fabric-f5 (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
work="$build/neg/fabric-f5"
checker="$root/tools/fabric/surface_check.py"
headers="engine/fabric/include/sl/fabric"
audit_re='f5 audit: ([a-z_]+) numerator \(bypassing\) ([0-9]+) denominator \(mutations\) ([0-9]+)'

trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-fabric-f5: FAIL: $1"
  exit 1
}

# a copy of what the surface lint reads
fresh_headers() {
  rm -rf "$work/headers"
  mkdir -p "$work/headers/$headers" "$work/headers/tools/fabric"
  cp -r "$root/$headers/." "$work/headers/$headers/"
  cp "$root/tools/fabric/surface-allow.txt" "$work/headers/tools/fabric/"
}

echo "neg-fabric-f5: control run (header copy, F5 audit of the working tree)"
fresh_headers
if ! out="$(python3 -I "$checker" --root "$work/headers" 2>&1)"; then
  echo "$out" | head -n 8
  fail "control: fabric-surface-check must exit 0 on the unmodified copy"
fi
cmake --preset debug >/dev/null
cmake --build --preset debug --target fabric_f5_ledger_test >/dev/null
ledger="$build/debug/tests/unit/fabric/fabric_f5_ledger_test"
if ! out="$("$ledger" 2>&1)"; then
  echo "$out" | tail -n 20
  fail "control: fabric_f5_ledger_test must pass on the working tree"
fi
grep -qE "$audit_re" <<<"$out" || fail "control: the F5 audit printed no 'f5 audit:' line"
while read -r line; do
  [[ "$line" =~ $audit_re ]] || continue
  [ "${BASH_REMATCH[2]}" -eq 0 ] || fail "control: ${BASH_REMATCH[1]} audit numerator ${BASH_REMATCH[2]}, expected 0"
done <<<"$out"

# (1) a public setter on value
fresh_headers
target="$work/headers/$headers/value.hpp"
anchor='  [[nodiscard]] const value &payload() const noexcept;'
grep -qxF "$anchor" "$target" || fail "1-setter: anchor line not found in value.hpp"
python3 -I - "$target" "$anchor" <<'PY'
import sys
path, anchor = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8") as fh:
    lines = fh.read().split("\n")
at = lines.index(anchor)
lines.insert(at + 1, "  value &at_mut();")
with open(path, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
PY
planted_line="$(grep -nxF '  value &at_mut();' "$target" | cut -d: -f1)"
[ -n "$planted_line" ] || fail "1-setter: the plant did not apply"
set +e
out="$(python3 -I "$checker" --root "$work/headers" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "1-setter: fabric-surface-check exited 0 with a public setter on value"
want="$headers/value.hpp:$planted_line: R-F5-SETTER: non-const member at_mut of value"
grep -qF "$want" <<<"$out" || {
  echo "$out" | head -n 8
  fail "1-setter: output does not name '$want'"
}
echo "neg-fabric-f5: 1-setter: exit $status, R-F5-SETTER at value.hpp:$planted_line (value::at_mut)"

# (2) the authorized_scope line removed from a copy of the transaction manager
tree="$work/tree"
rm -rf "$tree"
mkdir -p "$tree"
rsync -a --exclude /build --exclude /.git --exclude /evidence "$root"/ "$tree"/
src="$tree/engine/fabric/src/tx_manager.cpp"
[ "$(grep -c 'f5::authorized_scope' "$src")" -eq 1 ] || fail "2-scope: expected one f5::authorized_scope line"
grep -v 'f5::authorized_scope' "$src" >"$src.new"
mv "$src.new" "$src"
[ "$(grep -c 'f5::authorized_scope' "$src" || true)" -eq 0 ] || fail "2-scope: the scope line was not removed"
cmp -s "$src" "$root/engine/fabric/src/tx_manager.cpp" && fail "2-scope: the copy equals the original"
echo "neg-fabric-f5: 2-scope: removed the f5::authorized_scope line; building the copy (preset debug)"
(cd "$tree" && cmake --preset debug "-DSL_CCACHE_DIR=$build/.ccache" >"$work/configure.log" 2>&1) || {
  tail -n 15 "$work/configure.log"
  fail "2-scope: configure of the copy failed"
}
cmake --build "$tree/build/debug" --target fabric_f5_ledger_test >"$work/build.log" 2>&1 || {
  tail -n 15 "$work/build.log"
  fail "2-scope: build of the copy failed"
}
set +e
out="$("$tree/build/debug/tests/unit/fabric/fabric_f5_ledger_test" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "2-scope: fabric_f5_ledger_test passed with the authorized_scope line removed"
reported=0
while read -r line; do
  [[ "$line" =~ $audit_re ]] || continue
  backend="${BASH_REMATCH[1]}" bypassing="${BASH_REMATCH[2]}" mutations="${BASH_REMATCH[3]}"
  [ "$bypassing" -gt 0 ] || fail "2-scope: $backend audit reports bypassing $bypassing, expected > 0"
  [ "$mutations" -gt 0 ] || fail "2-scope: $backend audit reports mutations $mutations, expected > 0"
  echo "neg-fabric-f5: 2-scope: $backend bypassing $bypassing mutations $mutations"
  reported=$((reported + 1))
done <<<"$out"
[ "$reported" -ge 2 ] || {
  echo "$out" | tail -n 20
  fail "2-scope: expected an F5 audit line per backend, got $reported"
}
grep -qF 'site="memory_backend::commit"' <<<"$out" || fail "2-scope: no fabric/f5 event names memory_backend::commit"
echo "neg-fabric-f5: 2-scope: exit $status, the ledger test fails naming the bypassing site"
echo "neg-fabric-f5: ok (control exit 0; planted setter and removed scope line both failed and were named)"
