#!/usr/bin/env bash
# neg-deps-license (WO-032): each supply-chain rule of sldeps must be able to fail and must name the culprit.
# Works on a throwaway COPY of deps/, schemas/ and third_party/ under build/neg/deps-license:
#   0. control run on the clean copy: licenses, attest-check, vuln-check and check must exit 0
#   1. a GPL-3.0-only licence planted in the copy's manifest and lock: licenses and check fail, R-DEPS-LICENSE, doctest
#   2. doctest planted as unverified without a waiver: attest-check fails, R-DEPS-ATTEST, doctest; an expired waiver
#      still fails; a valid waiver passes
#   3. a verified package whose attestation no longer matches the lock: R-DEPS-ATTEST-STALE
#   4. the vendored LICENSE.txt removed: licenses fails, R-DEPS-LICFILE
#   5. an open finding against the locked doctest version: vuln-check and check fail, R-DEPS-VULN; a fixed one passes
# The original tree is never modified. SL_BUILD_DIR overrides the build directory (default: build).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

work="${SL_BUILD_DIR:-$root/build}/neg/deps-license"
sldeps=(python3 -I "$root/tools/deps/sldeps")
trap 'rm -rf "$work"' EXIT

fresh_copy() {
  rm -rf "$work"
  mkdir -p "$work"
  cp -r deps schemas third_party "$work"/
}

run() { # run <command> [args...]: sets $out and $status, never aborts
  set +e
  out="$("${sldeps[@]}" "$@" --root "$work" 2>&1)"
  status=$?
  set -e
}

expect_fail() { # expect_fail <label> <rule id> <culprit> <command> [args...]
  local label="$1" rule="$2" culprit="$3"
  shift 3
  run "$@"
  printf '%s\n' "$out" | grep -v ': note: ' | head -n 2
  if [ "$status" -eq 0 ]; then
    echo "neg-deps-license: FAIL: ${label}: '$*' exited 0"
    exit 1
  fi
  if ! printf '%s\n' "$out" | grep "$rule" | grep -q "$culprit"; then
    echo "neg-deps-license: FAIL: ${label}: '$*' failed (exit ${status}) but no line has ${rule} and ${culprit}"
    exit 1
  fi
  echo "neg-deps-license: ${label}: '$*' exit ${status}, ${rule} names ${culprit}"
}

expect_ok() { # expect_ok <label> <command> [args...]
  local label="$1"
  shift
  run "$@"
  if [ "$status" -ne 0 ]; then
    printf '%s\n' "$out" | head -n 3
    echo "neg-deps-license: FAIL: ${label}: '$*' must exit 0 but exited ${status}"
    exit 1
  fi
}

plant() { # plant <file in the copy> <sed script>: the file must change
  local before
  before="$(sha256sum "$work/$1")"
  sed -i "$2" "$work/$1"
  if [ "$before" = "$(sha256sum "$work/$1")" ]; then
    echo "neg-deps-license: FAIL: the plant '$2' did not change $1 (the tree no longer looks as this test expects)"
    exit 1
  fi
}

fresh_copy
echo "neg-deps-license: control run (clean copy)"
for cmd in licenses attest-check vuln-check check; do
  expect_ok "control" "$cmd"
done

fresh_copy
plant deps/deps.json '0,/"license": "MIT"/s//"license": "GPL-3.0-only"/'
plant deps/deps.lock.json '0,/"license": "MIT"/s//"license": "GPL-3.0-only"/'
expect_fail "planted GPL licence" R-DEPS-LICENSE doctest licenses
expect_fail "planted GPL licence" R-DEPS-LICENSE doctest check

fresh_copy
plant deps/deps.lock.json '0,/"verified_upstream": true/s//"verified_upstream": false/'
expect_fail "unverified, no waiver" R-DEPS-ATTEST doctest attest-check
cat >"$work/deps/attestation-waivers.json" <<'JSON'
{
  "schema": "sl-attest-waivers/1",
  "waivers": [{"package": "doctest", "reason": "neg test", "expires": "2026-01-01"}]
}
JSON
expect_fail "expired waiver" R-DEPS-ATTEST doctest attest-check --today 2026-10-17
expect_ok "valid waiver" attest-check --today 2025-12-31

fresh_copy
plant deps/attestations.json '0,/cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421/s//0000000000000000000000000000000000000000000000000000000000000000/'
expect_fail "stale attestation" R-DEPS-ATTEST-STALE doctest attest-check

fresh_copy
rm "$work/third_party/doctest/LICENSE.txt"
expect_fail "missing LICENSE file" R-DEPS-LICFILE doctest licenses

fresh_copy
cat >"$work/deps/vulnerabilities.json" <<'JSON'
{
  "schema": "sl-vulns/1",
  "findings": [
    {"package": "doctest", "version": "2.5.3", "advisory": "NEG-0001", "status": "open", "source": "neg test", "date": "2026-10-17"}
  ]
}
JSON
expect_fail "open finding" R-DEPS-VULN doctest vuln-check
expect_fail "open finding" R-DEPS-VULN doctest check
sed -i 's/"status": "open"/"status": "fixed"/' "$work/deps/vulnerabilities.json"
expect_ok "fixed finding" vuln-check

echo "neg-deps-license: ok (control exit 0; GPL licence, unverified package, stale attestation, missing LICENSE, open finding each fail and name doctest)"
