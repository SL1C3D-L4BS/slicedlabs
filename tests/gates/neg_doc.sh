#!/usr/bin/env bash
# neg-doc (WO-003, docs/arch set WO-035): a planted bad controlled document must make doc-check fail with the
# expected rule id. Two fixture sets, each planted at its own path in a throwaway tree:
#   tests/governance/docs/bad-*.md       -> docs/gov/SL-GOV-900-fixture.md
#   tests/governance/docs/arch/bad-*.md  -> docs/arch/SL-ARCH-900-fixture.md (uncited MUST, an eighth section)
# For each set (the rule id is the file name, upper-cased: bad-r-doc-cite.md expects R-DOC-CITE):
#   control: the set's good.md copied into the throwaway tree must exit 0
#   planted: each bad file copied into the same place must exit non-zero and print the rule id
# The throwaway tree is build/neg/doc (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/governance/cmd_check_docs.py"
work="${SL_BUILD_DIR:-$root/build}/neg/doc"

trap 'rm -rf "$work"' EXIT

total=0
# run_set <fixture directory> <target path relative to the throwaway tree>
run_set() {
  local fixtures="$1" rel="$2" target count=0 bad name rule out status
  rm -rf "$work"
  mkdir -p "$work/$(dirname "$rel")"
  target="$work/$rel"
  echo "neg-doc: control run (${fixtures#"$root"/}/good.md -> $rel)"
  cp "$fixtures/good.md" "$target"
  if ! python3 -I "$checker" --root "$work"; then
    echo "neg-doc: FAIL: control run must exit 0 but doc-check failed on ${fixtures#"$root"/}/good.md"
    exit 1
  fi
  for bad in "$fixtures"/bad-*.md; do
    [ -f "$bad" ] || continue
    name="$(basename "$bad" .md)"
    rule="$(printf '%s' "${name#bad-}" | tr '[:lower:]' '[:upper:]')"
    cp "$bad" "$target"
    set +e
    out="$(python3 -I "$checker" --root "$work" 2>&1)"
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
      echo "neg-doc: FAIL: $name ($rel) exited 0, expected non-zero with $rule"
      exit 1
    fi
    if ! printf '%s\n' "$out" | grep -q "$rule"; then
      echo "neg-doc: FAIL: $name ($rel) failed (exit $status) but the output does not name $rule"
      printf '%s\n' "$out" | head -n 4
      exit 1
    fi
    echo "neg-doc: $name ($rel) -> exit $status, $rule named"
    count=$((count + 1))
  done
  if [ "$count" -eq 0 ]; then
    echo "neg-doc: FAIL: no bad-*.md fixtures found in $fixtures"
    exit 1
  fi
  total=$((total + count))
}

run_set "$root/tests/governance/docs" "docs/gov/SL-GOV-900-fixture.md"
run_set "$root/tests/governance/docs/arch" "docs/arch/SL-ARCH-900-fixture.md"
echo "neg-doc: ok (control exit 0 per set, $total planted documents each failed with the expected rule id)"
