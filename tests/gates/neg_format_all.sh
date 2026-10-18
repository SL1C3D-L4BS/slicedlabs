#!/usr/bin/env bash
# neg-format-all (WO-024): one planted defect per non-C++ source kind must fail `just fmt-check-all`, exit
# non-zero and name its kind (and the planted file) as `kind:path:line:`.
# Works on a throwaway COPY of the working tree (tracked and untracked, ignored files left out) in
# build/neg/format-all; the original tree is never modified. SL_BUILD_DIR overrides the build directory.
#   1. control run: `just fmt-check-all` on the clean copy must exit 0
#   2. for each kind (cmake, python, shell, json, frontmatter): plant one defect, run the recipe, assert
#      a non-zero exit and a `kind:<planted path>:` line, then remove the plant
#   3. fix round trip: plant fixable defects, `just fmt-all` repairs them, `just fmt-check-all` exits 0
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/format-all"
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

git ls-files -z --cached --others --exclude-standard | tar --null --files-from=- -cf - | tar -xf - -C "$work"

cd "$work"
git init -q
git add -A

fail() {
  echo "neg-format-all: FAIL: $*"
  exit 1
}

echo "neg-format-all: control run (clean copy)"
if ! just fmt-check-all >/dev/null; then
  just fmt-check-all || true
  fail "control run must exit 0 but fmt-check-all failed on the clean copy"
fi

# plant <kind> <path>: the file content comes from stdin; assert the recipe fails and names kind and path.
plant() {
  local kind="$1" path="$2" status out
  mkdir -p "$(dirname "$path")"
  cat >"$path"
  git add -A
  set +e
  out="$(just fmt-check-all 2>&1)"
  status=$?
  set -e
  printf '%s\n' "$out" | grep -E "^${kind}:" | head -n 2
  if [ "$status" -eq 0 ]; then
    fail "${kind}: fmt-check-all exited 0 on a planted ${kind} defect (${path})"
  fi
  if ! printf '%s\n' "$out" | grep -qE "^${kind}:${path}:[0-9]+: "; then
    fail "${kind}: fmt-check-all failed (exit ${status}) but did not name '${kind}:${path}:LINE:'"
  fi
  echo "neg-format-all: ${kind}: ok (exit ${status}, ${path} named)"
  rm -f "$path"
  git add -A
}

printf 'if(TRUE)\n\tmessage("tab indent")  \nendif()\n' | plant cmake cmake/zz_plant_bad.cmake
printf 'x = "%0130d"\n' 0 | plant python tools/format/zz_plant_bad.py
printf '#!/usr/bin/env bash\necho trailing   \n' | plant shell scripts/zz_plant_bad.sh
printf '{"a":1,"b":[1,2]}\n' | plant json registry/zz_plant_bad.json
printf -- '---\nkind: GOV\nid: SL-GOV-999\ntitle: "Plant"\nstatus: Draft\nversion: 0.1.0\nowner: founder\nepisodes: []\ndepends_on: []\nsupersedes: []\ndecisions: []\n---\n# Plant\n' | plant frontmatter docs/gov/SL-GOV-999-plant.md

echo "neg-format-all: fix round trip (just fmt-all repairs trailing whitespace, tabs, final newline, JSON)"
printf 'if(TRUE)\n\tmessage("x")  \nendif()' >cmake/zz_plant_fix.cmake
printf 'x = 1   \n' >tools/format/zz_plant_fix.py
printf '#!/usr/bin/env bash\necho hi   \n' >scripts/zz_plant_fix.sh
printf '{"a":1,"b":[1,2]}' >registry/zz_plant_fix.json
git add -A
if just fmt-check-all >/dev/null 2>&1; then
  fail "fix round trip: the fixable plants were not detected"
fi
just fmt-all >/dev/null
if ! just fmt-check-all >/dev/null; then
  just fmt-check-all || true
  fail "fix round trip: fmt-all did not leave a clean tree"
fi
echo "neg-format-all: ok (control exit 0; cmake, python, shell, json, frontmatter each failed and were named; fmt-all repaired the fixable defects)"
