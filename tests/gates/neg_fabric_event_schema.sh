#!/usr/bin/env bash
# neg-fabric-event-schema (WO-047, SL-GOV-006 F6): an event envelope without a correlation field must make
# fabric-event-schema-check fail, naming the rule, the JSON pointer and the line.
#   control: `just fabric-event-schema-check` (builds, runs the fabric_events tests, validates the real dump) exits 0
#   (a) a copy of the dump whose last envelope lost correlation.scope: the checker exits non-zero and prints
#       R-EVT-CORRELATION, /correlation/scope and `:<line>:` of that envelope
#   (b) a copy whose first envelope has both subject and scope: R-EVT-ABOUT at line 1
# Works under build/neg/fabric-event-schema (SL_BUILD_DIR overrides build); the real dump is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
build="${SL_BUILD_DIR:-$root/build}"
checker="$root/tools/fabric/event_schema_check.py"
dump="$root/build/fabric/events.jsonl"
work="$build/neg/fabric-event-schema"

rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() {
  echo "neg-fabric-event-schema: FAIL: $1"
  exit 1
}

echo "neg-fabric-event-schema: control run (just fabric-event-schema-check)"
if ! just fabric-event-schema-check >"$work/control.log" 2>&1; then
  tail -n 20 "$work/control.log"
  fail "control fabric-event-schema-check must exit 0 on the real dump"
fi
count="$(wc -l <"$dump")"
[ "$count" -ge 1 ] || fail "the real dump has no event"

# (a) the last envelope loses correlation.scope
python3 -I -c '
import json, sys
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8").read().splitlines()
obj = json.loads(lines[-1])
del obj["correlation"]["scope"]
lines[-1] = json.dumps(obj, separators=(",", ":"))
open(dst, "w", encoding="utf-8").write("\n".join(lines) + "\n")
' "$dump" "$work/missing-scope.jsonl"
set +e
out="$(python3 -I "$checker" --file "$work/missing-scope.jsonl" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "a-missing-correlation-scope: the checker exited 0 on an envelope without correlation.scope"
for want in R-EVT-CORRELATION "/correlation/scope" "missing-scope.jsonl:$count:"; do
  grep -qF -- "$want" <<<"$out" || {
    echo "$out" | head -n 6
    fail "a-missing-correlation-scope: output does not name '$want'"
  }
done
echo "neg-fabric-event-schema: a-missing-correlation-scope: exit $status, names R-EVT-CORRELATION /correlation/scope line $count"

# (b) both subject and scope on the first envelope
python3 -I -c '
import json, sys
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8").read().splitlines()
obj = json.loads(lines[0])
obj["subject"] = "ent_050C0000000000000000000001"
obj["scope"] = "workspace"
lines[0] = json.dumps(obj, separators=(",", ":"))
open(dst, "w", encoding="utf-8").write("\n".join(lines) + "\n")
' "$dump" "$work/both.jsonl"
set +e
out="$(python3 -I "$checker" --file "$work/both.jsonl" 2>&1)"
status=$?
set -e
[ "$status" -ne 0 ] || fail "b-both-subject-and-scope: the checker exited 0 on an envelope with both"
for want in R-EVT-ABOUT "both.jsonl:1:"; do
  grep -qF -- "$want" <<<"$out" || {
    echo "$out" | head -n 6
    fail "b-both-subject-and-scope: output does not name '$want'"
  }
done
echo "neg-fabric-event-schema: b-both-subject-and-scope: exit $status, names R-EVT-ABOUT line 1"
echo "neg-fabric-event-schema: ok (control exit 0, 2 planted envelopes failed naming rule, pointer and line)"
