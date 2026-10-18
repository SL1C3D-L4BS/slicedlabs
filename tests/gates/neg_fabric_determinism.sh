#!/usr/bin/env bash
# neg-fabric-determinism (WO-041): the fabric-determinism comparator must fail, naming the vector, when one vector of
# the C++ corpus output changes.
#   control: two identical copies of the corpus output exit 0
#   (a) one hex digit of vector uint_300 changed in the gcc copy only: exit non-zero naming R-DET-COMPILER and
#       'uint_300'
#   (b) the text of vector f64_one_and_half changed in both copies: exit non-zero naming R-DET-CORPUS and
#       'f64_one_and_half'
# The base is build/fabric/corpus-clang.json when `just fabric-determinism` has run, else the output the emitter must
# produce, synthesized from accept.json. Copies live under build/neg/fabric-determinism (SL_BUILD_DIR overrides
# build); the repository is never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${SL_BUILD_DIR:-$root/build}"
checker="$root/tools/fabric/determinism.py"
accept="$root/tests/fixtures/fabric/slb/accept.json"
work="$build/neg/fabric-determinism"

trap 'rm -rf "$work"' EXIT

fresh() {
  rm -rf "$work"
  mkdir -p "$work"
  if [ -f "$build/fabric/corpus-clang.json" ]; then
    cp "$build/fabric/corpus-clang.json" "$work/clang.json"
  else
    python3 -I "$checker" --synthesize "$accept" "$work/clang.json"
  fi
  cp "$work/clang.json" "$work/gcc.json"
}

# plant <file> <vector> <field>: change one field of one row in a copy (the last hex digit, or the text)
plant() {
  python3 -I -c '
import json, sys
path, name, field = sys.argv[1:4]
doc = json.load(open(path, encoding="utf-8"))
row = next(r for r in doc["vectors"] if r["name"] == name)
if field == "hex":
    row["hex"] = row["hex"][:-1] + ("0" if row["hex"][-1] != "0" else "1")
else:
    row[field] = row[field] + "0"
body = ",\n".join(json.dumps(r, ensure_ascii=False) for r in doc["vectors"])
open(path, "w", encoding="utf-8").write("{\n\"format\": \"%s\",\n\"vectors\": [\n%s\n]\n}\n" % (doc["format"], body))
' "$1" "$2" "$3"
}

echo "neg-fabric-determinism: control run (identical copies)"
fresh
if ! python3 -I "$checker" "$work/clang.json" "$work/gcc.json" "$accept"; then
  echo "neg-fabric-determinism: FAIL: control run must exit 0 but the comparator failed on identical copies"
  exit 1
fi

cases=(
  "a-compiler-difference|gcc|uint_300|hex|R-DET-COMPILER"
  "b-corpus-difference|both|f64_one_and_half|text|R-DET-CORPUS"
)
count=0
for case in "${cases[@]}"; do
  IFS='|' read -r name where vector field rule <<<"$case"
  fresh
  plant "$work/gcc.json" "$vector" "$field"
  if [ "$where" = "both" ]; then
    plant "$work/clang.json" "$vector" "$field"
  fi
  set +e
  out="$(python3 -I "$checker" "$work/clang.json" "$work/gcc.json" "$accept" 2>&1)"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    echo "neg-fabric-determinism: FAIL: $name exited 0, expected non-zero naming $rule and '$vector'"
    exit 1
  fi
  if ! grep -q "$rule" <<<"$out" || ! grep -q "'$vector'" <<<"$out"; then
    echo "$out" | head -n 8
    echo "neg-fabric-determinism: FAIL: $name exited $status but the output does not name $rule and '$vector'"
    exit 1
  fi
  echo "neg-fabric-determinism: $name: exit $status, names $rule and '$vector'"
  count=$((count + 1))
done
echo "neg-fabric-determinism: ok (control exit 0, $count planted changes failed naming the vector)"
