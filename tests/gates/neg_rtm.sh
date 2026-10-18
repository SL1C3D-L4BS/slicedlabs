#!/usr/bin/env bash
# neg-rtm (WO-007): a planted defect in the traceability matrix must make rtm-check fail and name its rule id.
# Fixtures live in tests/governance/fixtures/rtm: good/ is a small self-contained tree (catalogue, registers,
# justfile); bad-<rule>.json replaces its SL-REG-003 register, bad-<rule>.md replaces its catalogue. The rule id is
# the file name upper-cased (bad-r-rtm-episode.json expects R-RTM-EPISODE, a dangling episode id).
#   control: the real repository and the good tree must exit 0
#   planted: each bad fixture, copied into a throwaway tree, must exit non-zero and print its rule id
# The throwaway tree is build/neg/rtm (SL_BUILD_DIR overrides build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixtures="$root/tests/governance/fixtures/rtm"
checker="$root/tools/governance/cmd_check_rtm.py"
work="${SL_BUILD_DIR:-$root/build}/neg/rtm"
register="registry/SL-REG-003-traceability.json"
catalogue="docs/gov/SL-GOV-002-requirements-catalogue.md"

rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

echo "neg-rtm: control run (repository)"
if ! python3 -I "$checker"; then
  echo "neg-rtm: FAIL: control run must exit 0 but rtm-check failed on the repository"
  exit 1
fi

count=0
for bad in "$fixtures"/bad-*.json "$fixtures"/bad-*.md; do
  [ -f "$bad" ] || continue
  base="$(basename "$bad")"
  name="${base%.*}"
  rule="$(printf '%s' "${name#bad-}" | tr '[:lower:]' '[:upper:]')"
  rm -rf "$work/tree"
  mkdir -p "$work/tree"
  cp -r "$fixtures/good/." "$work/tree/"
  if ! python3 -I "$checker" --root "$work/tree" >/dev/null; then
    echo "neg-rtm: FAIL: the good fixture tree must exit 0 before $base is planted"
    exit 1
  fi
  case "$base" in
    *.json) cp "$bad" "$work/tree/$register" ;;
    *) cp "$bad" "$work/tree/$catalogue" ;;
  esac
  set +e
  out="$(python3 -I "$checker" --root "$work/tree" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-rtm: FAIL: $base exited 0, expected non-zero with $rule"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep -q "$rule"; then
    echo "neg-rtm: FAIL: $base failed (exit $status) but the output does not name $rule"
    printf '%s\n' "$out" | head -n 4
    exit 1
  fi
  echo "neg-rtm: $base -> exit $status, $rule named"
  count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
  echo "neg-rtm: FAIL: no bad-* fixtures found in $fixtures"
  exit 1
fi
echo "neg-rtm: ok (control exit 0, $count planted defects each failed with the expected rule id)"
