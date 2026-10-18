#!/usr/bin/env bash
# neg-fabric-nothrow (WO-036): a planted `throw` in a copy of engine/fabric/src must make the nothrow lint fail
# naming N-THROW with file:line; a planted allow-list entry without a reason must fail naming N-ALLOW.
#   control  the copy (engine/fabric/src when it exists, else an empty directory) plus a clean file exits 0
#   (1) a new file with a throw on a known line -> engine/fabric/src/zz_planted_throw.cpp:<line>: N-THROW
#   (2) an allow-list entry for that file with no reason -> N-ALLOW, and the throw is still reported
# The throwaway tree is build/neg/fabric-nothrow (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lint="$root/tools/fabric/nothrow_lint.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-nothrow"
planted="engine/fabric/src/zz_planted_throw.cpp"

trap 'rm -rf "$work"' EXIT

fresh() {
  rm -rf "$work"
  mkdir -p "$work/tools/fabric" "$work/engine/fabric"
  if [ -d "$root/engine/fabric/src" ]; then cp -r "$root/engine/fabric/src" "$work/engine/fabric/src"; fi
  mkdir -p "$work/engine/fabric/src"
  cp "$root/tools/fabric/nothrow-allow.txt" "$work/tools/fabric/nothrow-allow.txt"
  printf 'int zz_clean(int x) {\n  return x + 1;\n}\n' >"$work/engine/fabric/src/zz_clean.cpp"
}

plant_throw() {
  printf 'int zz_planted(int x) {\n  if (x < 0) {\n    throw x;\n  }\n  return x;\n}\n' >"$work/$planted"
}

echo "neg-fabric-nothrow: control run (copy of engine/fabric/src, clean file added)"
fresh
if ! python3 -I "$lint" --root "$work"; then
  echo "neg-fabric-nothrow: FAIL: control run must exit 0 but the lint failed on the unmodified copy"
  exit 1
fi

fresh
plant_throw
set +e
out="$(python3 -I "$lint" --root "$work" 2>&1)"
status=$?
set -e
if [ "$status" -eq 0 ]; then
  echo "neg-fabric-nothrow: FAIL: the planted throw exited 0, expected non-zero with N-THROW"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q "^$planted:3: N-THROW"; then
  echo "neg-fabric-nothrow: FAIL: failed (exit $status) but no line reads $planted:3: N-THROW"
  printf '%s\n' "$out" | head -n 4
  exit 1
fi
echo "neg-fabric-nothrow: 1-throw -> exit $status, $planted:3: N-THROW named"

fresh
plant_throw
printf '%s:N-THROW\n' "$planted" >>"$work/tools/fabric/nothrow-allow.txt"
set +e
out="$(python3 -I "$lint" --root "$work" 2>&1)"
status=$?
set -e
if [ "$status" -eq 0 ] || ! printf '%s\n' "$out" | grep -q "N-ALLOW" || ! printf '%s\n' "$out" | grep -q "$planted:3: N-THROW"; then
  echo "neg-fabric-nothrow: FAIL: an allow entry without a reason must fail with N-ALLOW and keep the throw (exit $status)"
  printf '%s\n' "$out" | head -n 4
  exit 1
fi
echo "neg-fabric-nothrow: 2-allow-without-reason -> exit $status, N-ALLOW named, throw still reported"

echo "neg-fabric-nothrow: ok (control exit 0, the planted throw failed with N-THROW at file:line, a reasonless allow entry failed)"
