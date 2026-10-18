#!/usr/bin/env bash
# neg-fabric-gen (WO-045, FAB-INV-GEN-01): a hand edit of a generated file must fail fabric-gen-check naming the file.
#   control: a copy of the inputs and the committed generated files exits 0
#   (a) one generated header is edited in the copy: exit non-zero, output names R-GEN-STALE and that header
#   (b) one generated source is deleted in the copy: exit non-zero, output names R-GEN-MISSING and that source
# Each plant starts from a fresh copy under build/neg/fabric-gen (SL_BUILD_DIR overrides build); the repository is
# never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
slgen="$root/tools/fabric/slgen.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-gen"
header="engine/fabric/gen/include/sl/fabric/gen/fabric.history.gen.hpp"
source="engine/fabric/gen/src/fabric.manifest.gen.cpp"

trap 'rm -rf "$work"' EXIT

# a copy of what slgen reads: the schema sources, SL-ARCH-001, the generated tree and the type fixtures
fresh() {
  rm -rf "$work"
  mkdir -p "$work/schemas" "$work/docs" "$work/engine/fabric" "$work/tests/fixtures/fabric"
  cp -r "$root/schemas/fabric" "$work/schemas/fabric"
  cp -r "$root/docs/arch" "$work/docs/arch"
  cp -r "$root/engine/fabric/gen" "$work/engine/fabric/gen"
  cp -r "$root/tests/fixtures/fabric/types" "$work/tests/fixtures/fabric/types"
}

echo "neg-fabric-gen: control run (unchanged copy)"
fresh
if ! python3 -I "$slgen" check --root "$work"; then
  echo "neg-fabric-gen: FAIL: control run must exit 0 but fabric-gen-check failed on the unchanged copy"
  exit 1
fi

cases=(
  "a-header-edited|$header|R-GEN-STALE"
  "b-source-deleted|$source|R-GEN-MISSING"
)
count=0
for case in "${cases[@]}"; do
  IFS='|' read -r name file rule <<<"$case"
  fresh
  if [ "$rule" = "R-GEN-STALE" ]; then
    sed -i 's/struct revision {/struct revision { \/\/ hand edit/' "$work/$file"
  else
    rm "$work/$file"
  fi
  set +e
  out="$(python3 -I "$slgen" check --root "$work" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-fabric-gen: FAIL: $name exited 0, expected non-zero naming $rule and $file"
    exit 1
  fi
  if ! grep -q "$file: $rule" <<<"$out"; then
    echo "$out" | head -n 8
    echo "neg-fabric-gen: FAIL: $name exited $status but the output does not name $rule and $file"
    exit 1
  fi
  echo "neg-fabric-gen: $name: exit $status, names $rule and $file"
  count=$((count + 1))
done
echo "neg-fabric-gen: ok (control exit 0, $count planted edits failed naming the rule and the file)"
