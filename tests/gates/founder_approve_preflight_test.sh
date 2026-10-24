#!/usr/bin/env bash
# founder approve preflight (WO-127): in a throwaway clone under build/, scripts/founder/approve-001-080.sh
# --preflight-only must pass on a quiet machine and refuse (exit 1, naming the cause and --ignore-load) on a high
# load average, a live factory builder or a foreign ninja. It never runs an approval step. Run via
# `just founder-approve-preflight-test`.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$root/build"
work="$(mktemp -d "$root/build/founder-approve-preflight.XXXXXX")" || exit 1
ninja_pid=""
cleanup() {
  [ -z "$ninja_pid" ] || kill "$ninja_pid" 2>/dev/null
  rm -rf "$work"
}
trap cleanup EXIT

fail() {
  echo "founder-approve-preflight-test: FAIL: $*"
  exit 1
}

clone="$work/clone"
git clone -q --no-hardlinks "$root" "$clone" || fail "git clone of the repo at HEAD failed"
(cd "$clone" && git checkout -q -B main) || fail "could not put the clone on main"
mkdir -p "$clone/build/closeout"
echo "001 WO-127" >"$clone/build/closeout/ep-map.txt"

printf '0.50 0.40 0.30 1/200 12345\n' >"$work/load-quiet"
printf '5.00 4.00 3.00 1/200 12345\n' >"$work/load-high"
printf '{"entries": []}\n' >"$work/live-none.json"
printf '{"entries": [{"type": "builder", "ts": 1, "sid": "s", "tid": "", "aid": "a1b2c3d4e5f6"}]}\n' >"$work/live-one.json"

# preflight <load-file> <live-json> [args]: runs the clone's script; output in $work/out, exit code in $rc
preflight() {
  local load="$1" live="$2"
  shift 2
  (cd "$clone" && SL_LOADAVG_FILE="$load" SL_FACTORY_LIVE_JSON="$live" bash scripts/founder/approve-001-080.sh --preflight-only "$@" </dev/null) >"$work/out" 2>&1
  rc=$?
}
expect_pass() { # <label>
  [ "$rc" -eq 0 ] || {
    cat "$work/out"
    fail "$1: exited $rc, expected 0 (is the host busy with a build outside this test?)"
  }
  grep -q 'preflight ok' "$work/out" || {
    cat "$work/out"
    fail "$1: no 'preflight ok' line"
  }
}
expect_refuse() { # <label> <needle>...
  local label="$1" needle
  shift
  [ "$rc" -eq 1 ] || {
    cat "$work/out"
    fail "$label: exited $rc, expected 1"
  }
  for needle in "$@"; do
    grep -qF -- "$needle" "$work/out" || {
      cat "$work/out"
      fail "$label: output does not name '$needle'"
    }
  done
}

# 1 quiet machine passes
preflight "$work/load-quiet" "$work/live-none.json"
expect_pass "quiet machine"

# 2 load 5.00 refuses, naming the load and the override
preflight "$work/load-high" "$work/live-none.json"
expect_refuse "high load" "5.00" "--ignore-load"

# 3 --max-load 6 passes the same load
preflight "$work/load-high" "$work/live-none.json" --max-load 6
expect_pass "--max-load 6"

# 4 a live builder entry refuses, naming it
preflight "$work/load-quiet" "$work/live-one.json"
expect_refuse "live builder" "builder" "a1b2c3d4e5f6" "--ignore-load"

# 5 --ignore-load passes with a warning
preflight "$work/load-high" "$work/live-none.json" --ignore-load
expect_pass "--ignore-load"
grep -q 'WARNING.*5.00' "$work/out" || {
  cat "$work/out"
  fail "--ignore-load: no warning naming the load"
}

# 6 a stub ninja in its own process group refuses, naming ninja
cp "$(command -v sleep)" "$work/ninja"
setsid "$work/ninja" 600 >/dev/null 2>&1 &
ninja_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  ps -eo pid=,comm= | awk -v p="$ninja_pid" '$1 == p && $2 == "ninja" { f = 1 } END { exit !f }' && break
  sleep 0.2
done
preflight "$work/load-quiet" "$work/live-none.json"
expect_refuse "foreign ninja" "ninja" "--ignore-load"

echo "founder-approve-preflight-test: ok (quiet passes; high load, live builder and foreign ninja refuse naming the cause; --max-load and --ignore-load override)"
