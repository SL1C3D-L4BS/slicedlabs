#!/usr/bin/env bash
# neg-fabric-golden (WO-038): a drifted SLB/1 golden vector must fail fabric-golden-check naming the file and the vector.
#   control: a copy of tests/fixtures/fabric/slb exits 0
#   (a) one hex digit of the accept vector uint_300 is flipped in the copy: exit non-zero, output names accept.json
#       and uint_300
#   (b) the expected offset of the reject vector tag_00_in_list is changed in the copy: exit non-zero, output names
#       reject.json and tag_00_in_list
# Each plant starts from a fresh copy under build/neg/fabric-golden (SL_BUILD_DIR overrides build); the repository is
# never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checker="$root/tools/fabric/corpus.py"
work="${SL_BUILD_DIR:-$root/build}/neg/fabric-golden"

trap 'rm -rf "$work"' EXIT

fresh() {
  rm -rf "$work"
  mkdir -p "$work"
  cp -r "$root/tests/fixtures/fabric/slb" "$work/slb"
}

# plant <file> <vector> <field>: change one field of one vector in the copy (a hex digit, or an offset)
plant() {
  python3 -I -c '
import json, sys
path, name, field = sys.argv[1:4]
doc = json.load(open(path, encoding="utf-8"))
vector = next(v for v in doc["vectors"] if v["name"] == name)
if field == "hex":
    vector["hex"] = vector["hex"][:-1] + ("0" if vector["hex"][-1] != "0" else "1")
else:
    vector[field] += 1
rows = ",\n".join(json.dumps(v, ensure_ascii=False) for v in doc["vectors"])
text = "{\n\"format\": \"%s\",\n\"vectors\": [\n%s\n]\n}\n" % (doc["format"], rows)
open(path, "w", encoding="utf-8").write(text)
' "$work/slb/$1" "$2" "$3"
}

echo "neg-fabric-golden: control run (unmodified copy)"
fresh
if ! python3 -I "$checker" check --dir "$work/slb"; then
  echo "neg-fabric-golden: FAIL: control run must exit 0 but fabric-golden-check failed on the unmodified copy"
  exit 1
fi

cases=(
  "a-accept-hex-digit|accept.json|uint_300|hex"
  "b-reject-offset|reject.json|tag_00_in_list|offset"
)
count=0
for case in "${cases[@]}"; do
  IFS='|' read -r name file vector field <<<"$case"
  fresh
  plant "$file" "$vector" "$field"
  set +e
  out="$(python3 -I "$checker" check --dir "$work/slb" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-fabric-golden: FAIL: $name exited 0, expected non-zero naming $file and $vector"
    exit 1
  fi
  if ! grep -q "$file" <<<"$out" || ! grep -q "'$vector'" <<<"$out"; then
    echo "$out" | head -n 8
    echo "neg-fabric-golden: FAIL: $name exited $status but the output does not name $file and vector '$vector'"
    exit 1
  fi
  echo "neg-fabric-golden: $name: exit $status, names $file and '$vector'"
  count=$((count + 1))
done
echo "neg-fabric-golden: ok (control exit 0, $count planted drifts failed naming the file and the vector)"
