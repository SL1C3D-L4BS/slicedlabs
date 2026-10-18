#!/usr/bin/env bash
# neg-registry (WO-002): the registry rule engine must be able to fail, and name the culprit.
#   1. control run: tests/governance/fixtures/good-minimal.json must exit 0 with no findings
#   2. for every rule id the engine knows there must be tests/governance/fixtures/bad-<rule>.json
#   3. each bad fixture must exit non-zero and report exactly its own rule id
#   4. (WO-004) the real registry is clean, and each planted defect in an in-memory copy of it
#      (tools/governance/cmd_check_episodes.py --mutation-selftest) is named by its rule id or golden assert
# Read-only: nothing is written. Fixtures are static JSON, so no copy of the tree is needed.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

fixtures="tests/governance/fixtures"
checker=(python3 -I tools/governance/cmd_check_fixture.py)

echo "neg-registry: control run (${fixtures}/good-minimal.json)"
if ! out="$("${checker[@]}" "${fixtures}/good-minimal.json" 2>&1)"; then
  printf '%s\n' "$out" | head -n 4
  echo "neg-registry: FAIL: control run must exit 0 but the good fixture produced findings"
  exit 1
fi

mapfile -t rules < <("${checker[@]}" --list-rules)
if [ "${#rules[@]}" -eq 0 ]; then
  echo "neg-registry: FAIL: the checker lists no rules"
  exit 1
fi

failed=0
for rule in "${rules[@]}"; do
  fixture="${fixtures}/bad-${rule}.json"
  if [ ! -f "$fixture" ]; then
    echo "neg-registry: FAIL: ${rule} has no fixture ${fixture}"
    failed=1
    continue
  fi
  set +e
  found="$("${checker[@]}" --rules-only "$fixture" 2>/dev/null)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-registry: FAIL: ${fixture} exited 0 (the planted ${rule} defect was not caught)"
    failed=1
  elif [ "$found" != "$rule" ]; then
    echo "neg-registry: FAIL: ${fixture} reported [$(printf '%s' "$found" | tr '\n' ' ')] instead of exactly ${rule}"
    failed=1
  else
    echo "neg-registry: ${rule}: exit ${status}, ${rule} named"
  fi
done

# a bad fixture whose rule the engine no longer has is a stale test
for fixture in "${fixtures}"/bad-*.json; do
  name="${fixture##*/bad-}"
  name="${name%.json}"
  known=0
  for rule in "${rules[@]}"; do
    if [ "$rule" = "$name" ]; then known=1; fi
  done
  if [ "$known" -eq 0 ]; then
    echo "neg-registry: FAIL: ${fixture} names no known rule"
    failed=1
  fi
done

echo "neg-registry: real registry (registry/SL-REG-002-episodes.json) with planted defects"
set +e
mutout="$(python3 -I tools/governance/cmd_check_episodes.py --mutation-selftest 2>&1)"
mutstatus=$?
set -e
printf '%s\n' "$mutout"
if [ "$mutstatus" -ne 0 ]; then
  echo "neg-registry: FAIL: the real-registry mutation self-test exited ${mutstatus}"
  failed=1
fi

if [ "$failed" -ne 0 ]; then
  exit 1
fi
echo "neg-registry: ok (control exit 0, ${#rules[@]} planted defects each caught by exactly their rule id, real registry mutations named)"
