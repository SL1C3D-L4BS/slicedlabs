#!/bin/bash
# founder-push-test (WO-115): drives scripts/founder/push.sh against throwaway repositories under build/.
# Never touches origin or the network: every remote is a bare repository in the temp dir, reached through a
# repo-local url.<base>.insteadOf rewrite of a fake https/ssh URL. `just` is a stub on PATH (exit 0 / exit 1).
# Run via `just founder-push-test`.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
push="$root/scripts/founder/push.sh"
mkdir -p "$root/build"
work="$(mktemp -d "$root/build/founder-push-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# isolate git from the caller's environment and configuration
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR GIT_ALTERNATE_OBJECT_DIRECTORIES
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
export GIT_TERMINAL_PROMPT=0

# stub `just`: exit status from STUB_JUST_RC, every call logged
stubbin="$work/stubbin"
mkdir "$stubbin"
cat >"$stubbin/just" <<'STUB'
#!/bin/bash
echo "$*" >>"$STUB_JUST_LOG"
exit "${STUB_JUST_RC:-0}"
STUB
chmod +x "$stubbin/just"
export STUB_JUST_LOG="$work/just.log"
: >"$STUB_JUST_LOG"

passed=0
fail() {
  echo "founder-push-test: FAIL: $*"
  [ -f "${out:-/nonexistent}" ] && sed 's/^/  | /' "$out"
  exit 1
}
pass() {
  passed=$((passed + 1))
  echo "founder-push-test: ok: $*"
}

ncase=0
c="" repo="" bare="" out="" rc=0
# new_case [fake_url]: fresh bare repo + local repo with two commits on main; fake_url rewrites to the bare repo
new_case() {
  ncase=$((ncase + 1))
  c="$work/case$ncase"
  repo="$c/repo"
  bare="$c/remote.git"
  out="$c/out"
  mkdir -p "$c"
  git init -q --bare -b main "$bare"
  git init -q -b main "$repo"
  git -C "$repo" commit -q --allow-empty -m one
  git -C "$repo" commit -q --allow-empty -m two
  if [ "$#" -ge 1 ]; then
    git -C "$repo" config "url.file://$bare.insteadOf" "$1"
  fi
  : >"$STUB_JUST_LOG"
}

# run_push <stdin text> [push.sh args...]: runs in the case repo, output in $out, status in $rc
run_push() {
  local input="$1"
  shift
  rc=0
  (cd "$repo" && printf '%s' "$input" | PATH="$stubbin:$PATH" bash "$push" "$@") >"$out" 2>&1 || rc=$?
}

bare_main() { git -C "$bare" rev-parse --verify -q refs/heads/main || true; }
expect_refused() { # <message fragment>
  [ "$rc" -eq 1 ] || fail "$ncase: exit $rc, expected 1"
  grep -q "refused: .*$1" "$out" || fail "$ncase: refusal does not mention '$1'"
}

fake_https="https://github.com/test/repo.git"
fake_ssh="git@github.com:test/repo.git"
fake_ssh_url="ssh://git@github.com/test/repo.git"
actions="Actions: https://github.com/test/repo/actions"

# 1. dirty tree refused
new_case "$fake_https"
echo x >"$repo/f"
git -C "$repo" add f
git -C "$repo" commit -q -m f
echo y >"$repo/f"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not clean"
[ -z "$(bare_main)" ] || fail "dirty tree: something was pushed"
pass "dirty tree refused, nothing pushed"

# 2. untracked file refused
new_case "$fake_https"
echo y >"$repo/untracked"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not clean"
[ -z "$(bare_main)" ] || fail "untracked: something was pushed"
pass "untracked file refused, nothing pushed"

# 3. red fast refused
new_case "$fake_https"
rc=0
(cd "$repo" && printf 'yes\n' | STUB_JUST_RC=1 PATH="$stubbin:$PATH" bash "$push" --remote "$fake_https") >"$out" 2>&1 || rc=$?
expect_refused "just fast is red"
grep -qx 'fast' "$STUB_JUST_LOG" || fail "red fast: just fast was not run"
[ -z "$(bare_main)" ] || fail "red fast: something was pushed"
pass "red just fast refused, nothing pushed"

# 4. non-ancestor remote refused
new_case "$fake_https"
git clone -q "$bare" "$c/other" 2>/dev/null
git -C "$c/other" checkout -q -b main
git -C "$c/other" commit -q --allow-empty -m diverged
git -C "$c/other" push -q origin main
before="$(bare_main)"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not an ancestor"
[ "$(bare_main)" = "$before" ] || fail "non-ancestor: remote main moved"
pass "non-ancestor remote refused, remote main unchanged"

# 5. any answer but yes pushes nothing
for answer in $'y\n' $'no\n' $'YES\n' $'yes please\n' ''; do
  new_case "$fake_https"
  run_push "$answer" --remote "$fake_https"
  expect_refused "not 'yes'"
  [ -z "$(bare_main)" ] || fail "answer '$answer': something was pushed"
done
pass "answers y, no, YES, 'yes please' and EOF push nothing"

# 6. not on main, no origin
new_case
git -C "$repo" checkout -q -b feature
run_push $'yes\n' --remote "$fake_https"
expect_refused "not on branch main"
git -C "$repo" checkout -q main
run_push $'yes\n'
expect_refused "no remote named origin"
pass "branch other than main and missing origin refused"

# 7. yes pushes main (empty remote), no tag reaches it; then an incremental push
new_case "$fake_https"
git -C "$repo" tag v-test
git -C "$repo" tag -a -m annotated v-annotated
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "yes: exit $rc"
[ "$(bare_main)" = "$(git -C "$repo" rev-parse main)" ] || fail "yes: remote main differs from local main"
[ -z "$(git -C "$bare" tag)" ] || fail "yes: a tag reached the remote"
grep -q '^2 commit(s)\|: 2 commit(s) to push' "$out" || fail "yes: expected 2 commits listed"
grep -qx "$actions" "$out" || fail "yes: Actions URL line missing"
git -C "$repo" commit -q --allow-empty -m three
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "second push: exit $rc"
[ "$(bare_main)" = "$(git -C "$repo" rev-parse main)" ] || fail "second push: remote main differs"
grep -q ': 1 commit(s) to push' "$out" || fail "second push: expected 1 commit listed"
[ -z "$(git -C "$bare" tag)" ] || fail "second push: a tag reached the remote"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "up-to-date remote: exit $rc"
grep -q 'nothing to push' "$out" || fail "up-to-date remote should say nothing to push"
pass "yes pushes main only: remote main equals local main, no tag, incremental push lists 1 commit"

# 8. the Actions URL for ssh remotes
for u in "$fake_ssh" "$fake_ssh_url"; do
  new_case "$u"
  run_push $'yes\n' --remote "$u"
  [ "$rc" -eq 0 ] || fail "ssh $u: exit $rc"
  grep -qx "$actions" "$out" || fail "ssh $u: Actions URL line missing"
  [ "$(bare_main)" = "$(git -C "$repo" rev-parse main)" ] || fail "ssh $u: remote main differs"
done
pass "Actions URL printed for https and ssh remotes (scp-like and ssh://)"

# 9. origin mode: no --remote, origin from the repo config, upstream set on first push
new_case "$fake_https"
git -C "$repo" remote add origin "$fake_https"
run_push $'yes\n'
[ "$rc" -eq 0 ] || fail "origin mode: exit $rc"
[ "$(bare_main)" = "$(git -C "$repo" rev-parse main)" ] || fail "origin mode: remote main differs"
git -C "$repo" rev-parse --abbrev-ref 'main@{upstream}' >/dev/null 2>&1 || fail "origin mode: upstream not set"
grep -qx "$actions" "$out" || fail "origin mode: Actions URL line missing"
pass "origin mode pushes with -u and prints the Actions URL"

echo "founder-push-test: ok ($passed groups)"
