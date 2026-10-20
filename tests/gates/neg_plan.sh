#!/usr/bin/env bash
# neg-plan (WO-035, WO-102): planted Volume 04 and Volume 05 drift must make plan-check fail naming the volume's rule
# (R-PLAN-VOL04, R-PLAN-VOL05) and the episode.
#   control: a copy of the plan-check inputs (registry, views, docs/plan, docs/vol, work-orders, justfile) exits 0
#   (a) SL-EP-062 primary_wo names a work order that is not in its work_orders
#   (b) SL-EP-062 acceptance is a real recipe that its primary work order's acceptance list does not hold
#   (c) WO-045 traces SL-EP-062 but the SL-EP-062 record omits it from work_orders
#   (d) a second primary for WO-090 (primary of SL-EP-093): the SL-EP-089 record names it too; SL-EP-093 is named
# Each plant starts from a fresh copy. The throwaway tree is build/neg/plan (SL_BUILD_DIR overrides build); the
# repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/governance/cmd_plan_check.py"
work="${SL_BUILD_DIR:-$root/build}/neg/plan"

trap 'rm -rf "$work"' EXIT

fresh() {
  rm -rf "$work"
  mkdir -p "$work/registry" "$work/docs"
  cp "$root/registry/SL-REG-002-episodes.json" "$work/registry/"
  cp -r "$root/registry/views" "$work/registry/views"
  cp -r "$root/docs/plan" "$root/docs/vol" "$work/docs/"
  cp -r "$root/work-orders" "$work/work-orders"
  cp "$root/justfile" "$work/justfile"
}

# plant <spec> <old> <new>: replace exactly one occurrence in the copied spec, or fail the test
plant() {
  python3 -I -c '
import sys
path, old, new = sys.argv[1:4]
text = open(path, encoding="utf-8").read()
if text.count(old) != 1:
    sys.exit("neg-plan: FAIL: plant text occurs %d times in %s: %s" % (text.count(old), path, old))
open(path, "w", encoding="utf-8").write(text.replace(old, new))
' "$work/$1" "$2" "$3"
}

echo "neg-plan: control run (unmodified copy)"
fresh
if ! python3 -I "$checker" --root "$work"; then
  echo "neg-plan: FAIL: control run must exit 0 but plan-check failed on the unmodified copy"
  exit 1
fi

vol04="docs/vol/SL-VOL-04-SPEC.md"
vol05="docs/vol/SL-VOL-05-SPEC.md"
# name|spec|rule|episode|record|old|new
cases=(
  "a-primary-not-member|$vol04|R-PLAN-VOL04|SL-EP-062|\"episode\": \"SL-EP-062\", \"title\": \"Canonical type system\"|\"primary_wo\": \"WO-039\", \"acceptance\": \"just fabric-test type\"|\"primary_wo\": \"WO-036\", \"acceptance\": \"just fabric-test type\""
  "b-acceptance-not-in-primary|$vol04|R-PLAN-VOL04|SL-EP-062|\"episode\": \"SL-EP-062\", \"title\": \"Canonical type system\"|\"primary_wo\": \"WO-039\", \"acceptance\": \"just fabric-test type\"|\"primary_wo\": \"WO-039\", \"acceptance\": \"just fabric-test codec\""
  "c-tracing-wo-unlisted|$vol04|R-PLAN-VOL04|SL-EP-062|\"episode\": \"SL-EP-062\", \"title\": \"Canonical type system\"|\"work_orders\": [\"WO-035\", \"WO-039\", \"WO-045\"], \"primary_wo\": \"WO-039\"|\"work_orders\": [\"WO-035\", \"WO-039\"], \"primary_wo\": \"WO-039\""
  "d-second-primary-wo090|$vol05|R-PLAN-VOL05|SL-EP-093|\"episode\": \"SL-EP-089\", \"title\": \"Frame and simulation clocks\"|\"primary_wo\": \"WO-084\", \"acceptance\": \"just runtime-test clock\"|\"primary_wo\": \"WO-090\", \"acceptance\": \"just runtime-test clock\""
)
count=0
for case in "${cases[@]}"; do
  IFS='|' read -r name spec rule episode record old new <<<"$case"
  fresh
  if ! grep -qF "$record" "$work/$spec"; then
    echo "neg-plan: FAIL: $episode record not found in $spec"
    exit 1
  fi
  plant "$spec" "$old" "$new"
  set +e
  out="$(python3 -I "$checker" --root "$work" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-plan: FAIL: $name exited 0, expected non-zero with $rule for $episode"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep "$rule" | grep -q "$episode"; then
    echo "neg-plan: FAIL: $name failed (exit $status) but no line names both $rule and $episode"
    printf '%s\n' "$out" | head -n 4
    exit 1
  fi
  echo "neg-plan: $name -> exit $status, $rule $episode named"
  count=$((count + 1))
done
echo "neg-plan: ok (control exit 0, $count planted defects each failed with its volume rule naming its episode)"
