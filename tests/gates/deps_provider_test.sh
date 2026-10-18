#!/usr/bin/env bash
# deps-provider-test <provider> (WO-018): configure a throwaway build with SL_DEPS_PROVIDERS forced to one provider,
# assert build/<dir>/deps-resolved.json names that provider for doctest, then build and run sl_tests against it.
#   vendored | system   the real lock, the real provider
#   cpm-offline         a file:// doctest archive (tests/fixtures/deps/cpm, rebuilt here, sha256 checked against
#                       fixture.json) through a lock override; accept case, then a wrong url_hash must FAIL, then a
#                       remote url with an empty cache must be UNAVAILABLE naming the cache dir (offline, no network)
#   vcpkg-fixture       the fake vcpkg toolchain (tests/fixtures/deps/vcpkg) with a fake doctest 2.5.3 port; a lock
#                       that wants another version must be UNAVAILABLE
# Build directory: ${SL_BUILD_DIR:-build}/neg/provider-<provider>; removed on success, kept on failure.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

provider="${1:-}"
case "$provider" in
  vendored | system | cpm-offline | vcpkg-fixture) ;;
  *)
    echo "usage: deps_provider_test.sh <vendored|system|cpm-offline|vcpkg-fixture>" >&2
    exit 2
    ;;
esac

work="${SL_BUILD_DIR:-$root/build}/neg/provider-${provider}"
rm -rf "$work"
mkdir -p "$work"

fixtures="$root/tests/fixtures/deps"
want_provider="$provider"
extra=()

fail() {
  echo "deps-provider-test ${provider}: FAIL: $1"
  exit 1
}

# lock_with <out.json> <python statement on d (the lock) and argv[2:]>: a copy of the real lock, edited
lock_with() {
  local out="$1" edit="$2"
  shift 2
  python3 -I - "$root/deps/deps.lock.json" "$out" "$@" <<PY
import json, sys
d = json.load(open(sys.argv[1]))
$edit
json.dump(d, open(sys.argv[2], "w"), indent=1)
PY
}

# expect_configure_failure <label> <build dir> <pattern>...: configure must exit non-zero and say every pattern
expect_configure_failure() {
  local label="$1" dir="$2" pat out status
  shift 2
  set +e
  out="$(cmake --preset debug -B "$dir" "${extra[@]}" 2>&1)"
  status=$?
  set -e
  [ "$status" -ne 0 ] || {
    printf '%s\n' "$out" | tail -n 8
    fail "${label}: configure exited 0, expected a failure"
  }
  for pat in "$@"; do
    printf '%s\n' "$out" | grep -qF -- "$pat" || {
      printf '%s\n' "$out" | tail -n 12
      fail "${label}: configure failed (exit ${status}) but its output lacks '${pat}'"
    }
  done
  echo "deps-provider-test ${provider}: ${label}: rejected (exit ${status}), output names: $*"
}

case "$provider" in
  cpm-offline)
    want_provider=cpm
    archive="$work/doctest-2.5.3.tar"
    "$fixtures/cpm/make_fixture.sh" "$archive"
    have_sha="$(sha256sum "$archive" | cut -d' ' -f1)"
    recorded="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["archive_sha256"])' "$fixtures/cpm/fixture.json")"
    [ "$have_sha" = "$recorded" ] || fail "fixture archive sha256 ${have_sha} differs from tests/fixtures/deps/cpm/fixture.json (${recorded}); the doctest.h it is built from changed, or tar drifted"
    # a lock whose doctest cpm source is the file:// archive
    lock_with "$work/lock-good.json" 'd["packages"]["doctest"]["providers"]["cpm"].update(url=sys.argv[3], url_hash=sys.argv[4])' "file://$archive" "$have_sha"
    extra=("-DSL_DEPS_LOCK_OVERRIDE=$work/lock-good.json")

    # reject 1: the url_hash is not the archive's
    bad_sha="$(printf '%064d' 0)"
    lock_with "$work/lock-badhash.json" 'd["packages"]["doctest"]["providers"]["cpm"].update(url=sys.argv[3], url_hash=sys.argv[4])' "file://$archive" "$bad_sha"
    extra=("-DSL_DEPS_LOCK_OVERRIDE=$work/lock-badhash.json" -DSL_DEPS_PROVIDERS=cpm)
    expect_configure_failure "wrong url_hash" "$work/b-badhash" "integrity failure" "via cpm" "url_hash"
    # reject 2: a remote url, empty cache, SL_OFFLINE default ON: UNAVAILABLE, names the cache dir, no network attempted
    lock_with "$work/lock-remote.json" 'd["packages"]["doctest"]["providers"]["cpm"].update(url=sys.argv[3], url_hash=sys.argv[4])' "https://example.invalid/doctest-2.5.3.tar.gz" "$have_sha"
    extra=("-DSL_DEPS_LOCK_OVERRIDE=$work/lock-remote.json" -DSL_DEPS_PROVIDERS=cpm)
    expect_configure_failure "remote url, empty cache, offline" "$work/b-remote" "cache miss" "_cpm_cache" "SL_OFFLINE"
    # reject 3: a lock with no cpm pin (url_hash and git_commit null); the shipped lock has been pinned since 2026-10-17
    lock_with "$work/lock-unpinned.json" 'd["packages"]["doctest"]["providers"]["cpm"].update(url_hash=None, git_commit=None)'
    extra=("-DSL_DEPS_LOCK_OVERRIDE=$work/lock-unpinned.json" -DSL_DEPS_PROVIDERS=cpm)
    expect_configure_failure "unpinned lock" "$work/b-unpinned" "cpm provider unpinned" "deps-lock-refresh"

    extra=(-DSL_DEPS_PROVIDERS=cpm "-DSL_DEPS_LOCK_OVERRIDE=$work/lock-good.json")
    ;;
  vcpkg-fixture)
    want_provider=vcpkg
    lock_with "$work/lock-v9.json" 'd["packages"]["doctest"]["providers"]["vcpkg"]["version"] = "9.9.9"'
    extra=("-DCMAKE_TOOLCHAIN_FILE=$fixtures/vcpkg/fake-vcpkg.cmake" -DSL_DEPS_PROVIDERS=vcpkg "-DSL_DEPS_LOCK_OVERRIDE=$work/lock-v9.json")
    expect_configure_failure "locked version 9.9.9, port 2.5.3" "$work/b-version" "vcpkg port doctest is version '2.5.3'" "9.9.9"
    extra=("-DCMAKE_TOOLCHAIN_FILE=$fixtures/vcpkg/fake-vcpkg.cmake" -DSL_DEPS_PROVIDERS=vcpkg)
    ;;
  *)
    extra=(-DSL_DEPS_PROVIDERS="$provider")
    ;;
esac

echo "deps-provider-test ${provider}: configure with SL_DEPS_PROVIDERS=${want_provider}"
cmake --preset debug -B "$work" "${extra[@]}" >"$work.log" 2>&1 || {
  cat "$work.log"
  echo "deps-provider-test ${provider}: FAIL: configure failed"
  exit 1
}

resolved="$work/deps-resolved.json"
if [ ! -f "$resolved" ]; then
  echo "deps-provider-test ${provider}: FAIL: ${resolved} was not written"
  exit 1
fi
got="$(
  python3 -I - "$resolved" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
rows = [r for r in doc["resolved"] if r["package"] == "doctest"]
if len(rows) != 1 or not rows[0]["hash"] or not rows[0]["lock_sha256"]:
    print("bad")
else:
    print(rows[0]["provider"])
PY
)"
if [ "$got" != "$want_provider" ]; then
  cat "$resolved"
  echo "deps-provider-test ${provider}: FAIL: deps-resolved.json names provider '${got}' for doctest, expected '${want_provider}'"
  exit 1
fi

cmake --build "$work" --target sl_tests >>"$work.log" 2>&1 || {
  cat "$work.log"
  echo "deps-provider-test ${provider}: FAIL: sl_tests did not build against the ${provider} doctest"
  exit 1
}
ctest --test-dir "$work" -R '^sl_tests$' --no-tests=error >>"$work.log" 2>&1 || {
  cat "$work.log"
  echo "deps-provider-test ${provider}: FAIL: sl_tests did not pass"
  exit 1
}
rm -rf "$work" "$work.log"
echo "deps-provider-test ${provider}: ok (doctest resolved via ${want_provider}, sl_tests built and passed)"
