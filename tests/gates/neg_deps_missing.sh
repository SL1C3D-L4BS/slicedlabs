#!/usr/bin/env bash
# neg-deps-missing (WO-018): the resolver refuses a broken dependency and says which file and which provider.
# Works on a throwaway COPY of the tree under build/neg/deps-missing (build/ and dot-entries left out):
#   1. control: SL_DEPS_PROVIDERS=vendored configures (exit 0)
#   2. the vendored doctest header gets one extra line: configure with `vendored;system` must exit non-zero, name the
#      file and the provider `vendored`, and must NOT fall through to the system provider (which would have succeeded)
#   3. same corruption, ci defaults (SL_WARNINGS_AS_ERRORS=ON, no SL_DEPS_PROVIDERS): fails the same way
#   4. the header is deleted: configure fails naming the file (locked but missing)
#   5. header restored, SL_DEPS_PROVIDERS=cpm: configure must fail and list the providers tried (cpm, with its reason)
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/deps-missing"
header="third_party/doctest/doctest/doctest.h"
rm -rf "$work"
mkdir -p "$work/src"
trap 'rm -rf "$work"' EXIT

tar --exclude=./build --exclude='./.*' -cf - . | tar -xf - -C "$work/src"
cd "$work/src"

n=0
configure() { # configure <args...>: output in $out, exit status in $status
  n=$((n + 1))
  set +e
  out="$(cmake --preset debug -B "$work/b$n" "$@" 2>&1)"
  status=$?
  set -e
}

fail() {
  printf '%s\n' "$out" | tail -n 12
  echo "neg-deps-missing: FAIL: $1"
  exit 1
}

expect_failure() { # expect_failure <label> <pattern>...: configure failed and the output matches every pattern
  local label="$1" pat
  shift
  printf '%s\n' "$out" | grep -E 'sliced: deps|^  - ' | head -n 6
  [ "$status" -ne 0 ] || fail "${label}: configure exited 0"
  for pat in "$@"; do
    printf '%s\n' "$out" | grep -qE -- "$pat" || fail "${label}: configure failed (exit ${status}) but its output lacks '${pat}'"
  done
  echo "neg-deps-missing: ${label}: exit ${status}, output names: $*"
}

echo "neg-deps-missing: control run (clean copy, SL_DEPS_PROVIDERS=vendored)"
configure -DSL_DEPS_PROVIDERS=vendored
[ "$status" -eq 0 ] || fail "control run must configure but exited ${status}"

printf '// tampered\n' >>"$header"
configure '-DSL_DEPS_PROVIDERS=vendored;system'
expect_failure "corrupted header, vendored;system" "$header" "via vendored" "never falls through"
if printf '%s\n' "$out" | grep -q 'via system'; then
  fail "corrupted header: the resolver fell through to the system provider"
fi

configure -DSL_WARNINGS_AS_ERRORS=ON
expect_failure "corrupted header, ci defaults" "$header" "via vendored"

rm "$header"
configure -DSL_WARNINGS_AS_ERRORS=ON
expect_failure "deleted header, ci defaults" "$header" "via vendored" "missing"

cp "$root/$header" "$header"
configure -DSL_DEPS_PROVIDERS=cpm
expect_failure "SL_DEPS_PROVIDERS=cpm" "Providers tried" "- cpm: " "SL_OFFLINE"

echo "neg-deps-missing: ok (control exit 0; corrupt and deleted vendored header fail naming ${header} and vendored without fall-through; cpm lists providers tried)"
