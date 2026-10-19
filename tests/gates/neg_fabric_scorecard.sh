#!/usr/bin/env bash
# neg-fabric-scorecard (WO-062): the scorecard must fail naming the measure whose numerator is not 0.
#   control: the real scorecard input (written by fabric_conformance_gate1_test) passes tools/fabric/scorecard.py
#   (a) a copy with F5 bypassing = 1 must exit 1 naming F5
#   (b) a copy with F2 numerator = 1 must exit 1 naming F2
# Works under build/neg/fabric-scorecard (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
scorer="$root/tools/fabric/scorecard.py"
work="$build/neg/fabric-scorecard"

trap 'rm -rf "$work"' EXIT

cmake --preset debug >/dev/null
cmake --build --preset debug --target fabric_conformance_gate1_test >/dev/null
rm -rf "$work"
mkdir -p "$work"
SL_FABRIC_SCORECARD_INPUT="$work/input.json" ctest --preset debug -R '^fabric_conformance_gate1_test$' \
  --no-tests=error --output-on-failure >/dev/null

fail() {
  echo "neg-fabric-scorecard: FAIL: $1"
  exit 1
}

# runs the scorer on input $1; sets out and status
run_score() {
  set +e
  out="$(python3 -I "$scorer" "$1" --out "$work/scorecard.json" 2>&1)"
  status=$?
  set -e
}

echo "neg-fabric-scorecard: control run (real conformance input)"
run_score "$work/input.json"
[ "$status" -eq 0 ] || {
  echo "$out"
  fail "control run must exit 0 but fabric-scorecard failed on the real input"
}

# (a) one store mutation bypassed the transaction manager
grep -q '"bypassing": 0' "$work/input.json" || fail "a-f5-bypass: the input has no \"bypassing\": 0 to plant"
sed 's/"bypassing": 0/"bypassing": 1/' "$work/input.json" >"$work/f5.json"
run_score "$work/f5.json"
[ "$status" -eq 1 ] || fail "a-f5-bypass: fabric-scorecard exited $status with bypassing = 1, expected 1"
for want in "FAIL F5:" "1 of" "bypassed the transaction manager"; do
  grep -qF "$want" <<<"$out" || {
    echo "$out"
    fail "a-f5-bypass: output does not name '$want'"
  }
done
echo "neg-fabric-scorecard: a-f5-bypass: exit $status, names F5"

# (b) a domain with a second owner
grep -q '"f2": {"numerator": 0' "$work/input.json" || fail "b-f2-owner: the input has no F2 numerator 0 to plant"
sed 's/"f2": {"numerator": 0/"f2": {"numerator": 1/' "$work/input.json" >"$work/f2.json"
run_score "$work/f2.json"
[ "$status" -eq 1 ] || fail "b-f2-owner: fabric-scorecard exited $status with F2 numerator 1, expected 1"
grep -qF "FAIL F2:" <<<"$out" || {
  echo "$out"
  fail "b-f2-owner: output does not name F2"
}
echo "neg-fabric-scorecard: b-f2-owner: exit $status, names F2"
echo "neg-fabric-scorecard: ok (control exit 0, 2 planted numerators failed naming F5 and F2)"
