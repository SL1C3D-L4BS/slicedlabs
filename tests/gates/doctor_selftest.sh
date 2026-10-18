#!/usr/bin/env bash
# doctor selftest (WO-013): the control run passes; with cmake absent from the tool PATH the doctor
# must exit 1 and name cmake and `sudo pacman -S cmake`. Run via `just doctor-selftest`.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
doctor="$root/scripts/doctor"
work="$(mktemp -d "${TMPDIR:-/tmp}/doctor-selftest.XXXXXX")" || exit 1
trap 'rm -rf "$work"' EXIT

fail() {
  echo "doctor-selftest: FAIL: $*"
  exit 1
}

# control: the real PATH
bash "$doctor" >"$work/control.out" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { cat "$work/control.out"; fail "control run exited $rc, expected 0 (install what it lists)"; }

# restricted PATH dir: a symlink to every required tool except cmake
mkdir "$work/bin"
python3 -I - "$root/toolchain/environments.json" >"$work/required.txt" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    for tool in json.load(fh)["tools"]:
        if tool["tier"] == "required":
            print(tool.get("binary", tool["name"]))
PY
count=0
while IFS= read -r name; do
  [ "$name" = cmake ] && continue
  src="$(command -v "$name")" || fail "required tool $name not found for the symlink farm"
  ln -s "$src" "$work/bin/$name"
  count=$((count + 1))
done <"$work/required.txt"
[ "$count" -gt 0 ] || fail "no required tools read from environments.json"
[ ! -e "$work/bin/cmake" ] || fail "cmake must be absent from the restricted PATH"

SL_DOCTOR_PATH="$work/bin" bash "$doctor" >"$work/missing.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] || { cat "$work/missing.out"; fail "restricted run exited $rc, expected 1"; }
grep -qE '^cmake +required +- +[^ ]+ +missing' "$work/missing.out" || { cat "$work/missing.out"; fail "restricted run did not report cmake missing"; }
grep -qxF 'sudo pacman -S cmake' "$work/missing.out" || { cat "$work/missing.out"; fail "restricted run did not print 'sudo pacman -S cmake'"; }
[ "$(grep -c '^sudo pacman -S ' "$work/missing.out")" -eq 1 ] || { cat "$work/missing.out"; fail "expected exactly one sudo pacman line"; }

echo "doctor-selftest: ok (control exit 0; cmake absent -> exit 1 naming cmake)"
