#!/usr/bin/env bash
# neg-deps (WO-017): a corrupted vendored hash must make `sldeps check` fail and name the vendored file (C06).
# Works on a throwaway COPY of deps/, schemas/ and third_party/ under build/neg/deps:
#   1. control run on the clean copy: must exit 0
#   2. plant 1: the lock's sha256 for third_party/cpm/CPM.cmake is corrupted: must exit non-zero, name the file
#   3. plant 2: a byte is appended to the copied third_party/cpm/CPM.cmake: must exit non-zero, name the file
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/deps"
sldeps=(python3 -I "$root/tools/deps/sldeps" check --root "$work")
target="third_party/cpm/CPM.cmake"
good="49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa"
bad="0000000000000000000000000000000000000000000000000000000000000000"

fresh_copy() {
  rm -rf "$work"
  mkdir -p "$work"
  cp -r deps schemas third_party "$work"/
}
trap 'rm -rf "$work"' EXIT

expect_named_failure() {
  local label="$1" out status
  set +e
  out="$("${sldeps[@]}" 2>&1)"
  status=$?
  set -e
  printf '%s\n' "$out" | grep -v ': note: ' | head -n 3
  if [ "$status" -eq 0 ]; then
    echo "neg-deps: FAIL: ${label}: deps-check exited 0 on a corrupted vendored file"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep -q "$target"; then
    echo "neg-deps: FAIL: ${label}: deps-check failed (exit ${status}) but its output does not name ${target}"
    exit 1
  fi
  echo "neg-deps: ${label}: exit ${status}, ${target} named"
}

fresh_copy
echo "neg-deps: control run (clean copy)"
if ! "${sldeps[@]}" >/dev/null; then
  echo "neg-deps: FAIL: control run must exit 0 but deps-check failed on the clean copy"
  exit 1
fi

fresh_copy
sed -i "s/${good}/${bad}/" "$work/deps/deps.lock.json"
if ! grep -q "$bad" "$work/deps/deps.lock.json"; then
  echo "neg-deps: FAIL: the lock hash was not corrupted (the lock no longer carries ${good})"
  exit 1
fi
expect_named_failure "planted lock hash"

fresh_copy
printf '\n# tampered\n' >>"$work/$target"
expect_named_failure "planted file change"

echo "neg-deps: ok (control exit 0, both plants fail and name ${target})"
