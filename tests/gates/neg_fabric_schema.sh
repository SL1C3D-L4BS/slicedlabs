#!/usr/bin/env bash
# neg-fabric-schema (WO-040): editing a released type or reusing a reserved field number must fail fabric-schema-check
# naming the rule and the type.
#   control: a copy with the fixture modules added and locked (schema_check.py lock) exits 0
#   (a) a released field type is edited in place (content.shapes.scene 1.1, field 1 string -> bytes): exit non-zero,
#       output names R-SCH-RELEASED and content.shapes.scene
#   (b) a new version 1.2 of content.shapes.scene adds a field with its reserved number 4: exit non-zero, output names
#       R-SCH-REUSE and content.shapes.scene
# Each plant starts from a fresh copy under build/neg/fabric-schema (SL_BUILD_DIR overrides build); the repository is
# never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/fabric/schema_check.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-schema"
type_name="content.shapes.scene"
module_file="schemas/fabric/content.shapes.sltype.json"

trap 'rm -rf "$work"' EXIT

# a copy of the repository files the checker reads, plus the fixture modules, locked
fresh() {
  rm -rf "$work"
  mkdir -p "$work/schemas/fabric" "$work/docs"
  cp "$root/schemas/fabric/sl-type-def.schema.json" "$root/schemas/fabric/schema.lock.json" "$work/schemas/fabric/"
  cp -r "$root/docs/arch" "$work/docs/arch"
  cp "$root"/tests/fixtures/fabric/schema/modules/*.sltype.json "$work/schemas/fabric/"
  python3 -I "$checker" lock --root "$work" >/dev/null
}

# plant <a|b>: edit the scene type of the copy
plant() {
  python3 -I -c '
import json, sys
path, which = sys.argv[1:3]
doc = json.load(open(path, encoding="utf-8"))
scene = next(t for t in doc["types"] if t["name"] == "content.shapes.scene")
if which == "a":
    next(f for f in scene["fields"] if f["no"] == 1)["type"] = "bytes"
else:
    scene["version"] = "1.2"
    scene["fields"].append({"no": 4, "name": "again", "type": "bool", "presence": "optional"})
open(path, "w", encoding="utf-8").write(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
' "$work/$module_file" "$1"
}

echo "neg-fabric-schema: control run (fixture modules added and locked)"
fresh
if ! python3 -I "$checker" check --root "$work"; then
  echo "neg-fabric-schema: FAIL: control run must exit 0 but fabric-schema-check failed on the locked copy"
  exit 1
fi

cases=(
  "a-released-field-type|a|R-SCH-RELEASED"
  "b-reserved-number-reused|b|R-SCH-REUSE"
)
count=0
for case in "${cases[@]}"; do
  IFS='|' read -r name which rule <<<"$case"
  fresh
  plant "$which"
  set +e
  out="$(python3 -I "$checker" check --root "$work" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-fabric-schema: FAIL: $name exited 0, expected non-zero naming $rule and $type_name"
    exit 1
  fi
  if ! grep -q "$rule" <<<"$out" || ! grep -q "$type_name" <<<"$out"; then
    echo "$out" | head -n 8
    echo "neg-fabric-schema: FAIL: $name exited $status but the output does not name $rule and $type_name"
    exit 1
  fi
  echo "neg-fabric-schema: $name: exit $status, names $rule and $type_name"
  count=$((count + 1))
done
echo "neg-fabric-schema: ok (control exit 0, $count planted edits failed naming the rule and the type)"
