#!/usr/bin/env bash
# neg-doc (WO-003): a planted bad controlled document must make doc-check fail with the expected rule id.
# For each tests/governance/docs/bad-<rule>.md (the rule id is the file name, upper-cased: bad-r-doc-cite.md
# expects R-DOC-CITE):
#   control: tests/governance/docs/good.md copied into a throwaway tree must exit 0
#   planted: the bad file copied into the same place must exit non-zero and print the rule id
# The throwaway tree is build/neg/doc (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixtures="$root/tests/governance/docs"
checker="$root/tools/governance/cmd_check_docs.py"
work="${SL_BUILD_DIR:-$root/build}/neg/doc"

rm -rf "$work"
mkdir -p "$work/docs/gov"
trap 'rm -rf "$work"' EXIT

target="$work/docs/gov/SL-GOV-900-fixture.md"

echo "neg-doc: control run (good.md)"
cp "$fixtures/good.md" "$target"
if ! python3 -I "$checker" --root "$work"; then
  echo "neg-doc: FAIL: control run must exit 0 but doc-check failed on good.md"
  exit 1
fi

count=0
for bad in "$fixtures"/bad-*.md; do
  name="$(basename "$bad" .md)"
  rule="$(printf '%s' "${name#bad-}" | tr '[:lower:]' '[:upper:]')"
  cp "$bad" "$target"
  set +e
  out="$(python3 -I "$checker" --root "$work" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-doc: FAIL: $name exited 0, expected non-zero with $rule"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep -q "$rule"; then
    echo "neg-doc: FAIL: $name failed (exit $status) but the output does not name $rule"
    printf '%s\n' "$out" | head -n 4
    exit 1
  fi
  echo "neg-doc: $name -> exit $status, $rule named"
  count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
  echo "neg-doc: FAIL: no bad-*.md fixtures found in $fixtures"
  exit 1
fi
echo "neg-doc: ok (control exit 0, $count planted documents each failed with the expected rule id)"
