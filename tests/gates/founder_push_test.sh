#!/bin/bash
# founder-push-test (WO-115, WO-119): drives scripts/founder/push.sh against throwaway repositories under build/.
# Never touches origin or the network: every remote is a bare repository in the temp dir, reached through a
# repo-local url.<base>.insteadOf rewrite of a fake https/ssh URL (or a file:// URL). `just` is a stub on PATH.
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

# stub `just`: every call logged; exit status from STUB_JUST_RC; STUB_JUST_COMMIT=1 commits to the current branch
# during the run; STUB_JUST_ADVANCE=<repo> commits in that repo and pushes it to its origin during the run
stubbin="$work/stubbin"
mkdir "$stubbin"
cat >"$stubbin/just" <<'STUB'
#!/bin/bash
echo "$*" >>"$STUB_JUST_LOG"
# stdin must be closed for just: any byte here would swallow the typed answer
if [ -n "$(cat)" ]; then
  echo "stub just: got bytes on stdin" >&2
  exit 97
fi
if [ "${STUB_JUST_COMMIT:-0}" = 1 ]; then
  git commit -q --allow-empty -m moved-during-fast
fi
if [ -n "${STUB_JUST_ADVANCE:-}" ]; then
  git -C "$STUB_JUST_ADVANCE" commit -q --allow-empty -m advanced-during-fast
  git -C "$STUB_JUST_ADVANCE" push -q origin main
fi
exit "${STUB_JUST_RC:-0}"
STUB
chmod +x "$stubbin/just"
export STUB_JUST_LOG="$work/just.log"
: >"$STUB_JUST_LOG"

# a PATH with git and nothing else, for the missing-just case
nojust="$work/nojust"
mkdir "$nojust"
ln -s "$(command -v git)" "$nojust/git"
bashbin="$(command -v bash)"

passed=0
fail() {
  echo "founder-push-test: FAIL: $*"
  if [ -f "${out:-/nonexistent}" ]; then sed 's/^/  | /' "$out"; fi
  exit 1
}
pass() {
  passed=$((passed + 1))
  echo "founder-push-test: ok: $*"
}

ncase=0
c="" repo="" bare="" out="" rc=0
push_path="$stubbin:$PATH"
extra_env=()
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
  push_path="$stubbin:$PATH"
  extra_env=()
  : >"$STUB_JUST_LOG"
}

# new_other: a second repo with its own history whose origin is the case's bare repo
new_other() {
  git init -q -b main "$c/other"
  git -C "$c/other" commit -q --allow-empty -m other-root
  git -C "$c/other" remote add origin "file://$bare"
}

# run_push <stdin text> [push.sh args...]: runs in the case repo, output in $out, status in $rc
run_push() {
  local input="$1"
  shift
  rc=0
  (cd "$repo" && printf '%s' "$input" | env PATH="$push_path" "${extra_env[@]}" "$bashbin" "$push" "$@") >"$out" 2>&1 || rc=$?
}

bare_main() { git -C "$bare" rev-parse --verify -q refs/heads/main || true; }
local_main() { git -C "$repo" rev-parse main; }
expect_refused() { # <message fragment>
  [ "$rc" -eq 1 ] || fail "case $ncase: exit $rc, expected 1"
  grep -q "refused: .*$1" "$out" || fail "case $ncase: refusal does not mention '$1'"
}
expect_no_fast() { [ ! -s "$STUB_JUST_LOG" ] || fail "case $ncase: just ran before the refusal"; }
expect_one_fast() {
  [ "$(cat "$STUB_JUST_LOG")" = fast ] || fail "case $ncase: just fast must run exactly once (log: $(tr '\n' ' ' <"$STUB_JUST_LOG"))"
}
expect_no_actions() { ! grep -q '^Actions:' "$out" || fail "case $ncase: an Actions line was printed"; }
expect_actions() { # sha
  grep -qx "Actions: https://github.com/test/repo/commit/$1/checks" "$out" || fail "case $ncase: Actions line for $1 missing"
}

fake_https="https://github.com/test/repo.git"
fake_ssh="git@github.com:test/repo.git"
fake_ssh_url="ssh://git@github.com/test/repo.git"

# 1. dirty tree refused
new_case "$fake_https"
echo x >"$repo/f"
git -C "$repo" add f
git -C "$repo" commit -q -m f
echo y >"$repo/f"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not clean"
expect_no_fast
[ -z "$(bare_main)" ] || fail "dirty tree: something was pushed"
pass "dirty tree refused before fast, nothing pushed"

# 2. untracked file refused
new_case "$fake_https"
echo y >"$repo/untracked"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not clean"
expect_no_fast
[ -z "$(bare_main)" ] || fail "untracked: something was pushed"
pass "untracked file refused before fast, nothing pushed"

# 3. red fast refused
new_case "$fake_https"
extra_env=(STUB_JUST_RC=1)
run_push $'yes\n' --remote "$fake_https"
expect_refused "just fast is red"
expect_one_fast
[ -z "$(bare_main)" ] || fail "red fast: something was pushed"
pass "red just fast refused, nothing pushed"

# 4a. a remote main this clone does not hold: "not in this clone", with the hint to fetch
new_case "$fake_https"
new_other
git -C "$c/other" push -q origin main
before="$(bare_main)"
run_push $'yes\n' --remote "$fake_https"
expect_refused "remote main $before is not in this clone; fetch from origin, then retry"
expect_no_fast
[ "$(bare_main)" = "$before" ] || fail "unknown remote: remote main moved"
pass "unknown remote main refused with the fetch hint, before fast"

# 4b. a known remote main that diverged (local history rewritten), and 4c. a remote ahead of local
new_case "$fake_https"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "4b setup push: exit $rc"
before="$(bare_main)"
git -C "$repo" reset -q --hard HEAD~1
git -C "$repo" commit -q --allow-empty -m rewritten
: >"$STUB_JUST_LOG"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not an ancestor"
! grep -q 'not in this clone' "$out" || fail "diverged known remote reported as unknown"
expect_no_fast
[ "$(bare_main)" = "$before" ] || fail "diverged remote: remote main moved"
git -C "$repo" reset -q --hard HEAD~1
: >"$STUB_JUST_LOG"
run_push $'yes\n' --remote "$fake_https"
expect_refused "not an ancestor"
expect_no_fast
[ "$(bare_main)" = "$before" ] || fail "remote ahead: remote main moved"
pass "known diverged remote and remote ahead of local refused as not an ancestor, before fast"

# 5. any answer but a bare yes pushes nothing
for answer in $'y\n' $'no\n' $'YES\n' $'yes please\n' $' yes \n' $'\tyes\n' ''; do
  new_case "$fake_https"
  run_push "$answer" --remote "$fake_https"
  expect_refused "not 'yes'"
  expect_one_fast
  [ -z "$(bare_main)" ] || fail "answer '$answer': something was pushed"
done
pass "answers y, no, YES, 'yes please', ' yes ', tab-yes and EOF push nothing"

# 5b. a CRLF terminal and an unterminated line still count as yes
for answer in $'yes\r\n' 'yes'; do
  new_case "$fake_https"
  run_push "$answer" --remote "$fake_https"
  [ "$rc" -eq 0 ] || fail "answer yes variant: exit $rc"
  [ "$(bare_main)" = "$(local_main)" ] || fail "answer yes variant: remote main differs"
done
pass "'yes' with CRLF and without a newline pushes"

# 6. not on main, no origin, no just
new_case
git -C "$repo" checkout -q -b feature
run_push $'yes\n' --remote "$fake_https"
expect_refused "not on branch main"
expect_no_fast
git -C "$repo" checkout -q main
run_push $'yes\n'
expect_refused "no remote named origin"
expect_no_fast
push_path="$nojust"
run_push $'yes\n' --remote "$fake_https"
expect_refused "just is not installed"
pass "branch other than main, missing origin and missing just refused before fast"

# 7. yes pushes the checked sha: tags never reach the remote even with push.followTags=true
new_case "$fake_https"
git -C "$repo" config push.followTags true
git -C "$repo" tag v-test
git -C "$repo" tag -a -m annotated v-annotated
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "yes: exit $rc"
[ "$(bare_main)" = "$(local_main)" ] || fail "yes: remote main differs from local main"
[ -z "$(git -C "$bare" tag)" ] || fail "yes: a tag reached the remote"
expect_one_fast
grep -q ': 2 commit(s) up to ' "$out" || fail "yes: expected 2 commits listed"
grep -qE "^[0-9a-f]{40} [A-Za-z?] (one|two)$" "$out" || fail "yes: commit list is not full-hash with signature status"
expect_actions "$(local_main)"
git -C "$repo" commit -q --allow-empty -m three
: >"$STUB_JUST_LOG"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "second push: exit $rc"
[ "$(bare_main)" = "$(local_main)" ] || fail "second push: remote main differs"
grep -q ': 1 commit(s) up to ' "$out" || fail "second push: expected 1 commit listed"
[ -z "$(git -C "$bare" tag)" ] || fail "second push: a tag reached the remote"
: >"$STUB_JUST_LOG"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "up-to-date remote: exit $rc"
grep -q 'nothing to push' "$out" || fail "up-to-date remote should say nothing to push"
pass "yes pushes the checked sha: remote main equals local main, no tag despite push.followTags, 1 fast per run"

# 8. the Actions line: github.com only, tied to the pushed sha
for u in "$fake_ssh" "$fake_ssh_url"; do
  new_case "$u"
  run_push $'yes\n' --remote "$u"
  [ "$rc" -eq 0 ] || fail "ssh $u: exit $rc"
  expect_actions "$(local_main)"
  [ "$(bare_main)" = "$(local_main)" ] || fail "ssh $u: remote main differs"
done
new_case
run_push $'yes\n' --remote "file://$bare"
[ "$rc" -eq 0 ] || fail "file remote: exit $rc"
[ "$(bare_main)" = "$(local_main)" ] || fail "file remote: remote main differs"
expect_no_actions
pass "Actions line for https and ssh (scp-like, ssh://) github.com remotes; none for a file:// remote"

# 9. origin mode: no --remote, origin from the repo config, upstream set on first push
new_case "$fake_https"
git -C "$repo" remote add origin "$fake_https"
run_push $'yes\n'
[ "$rc" -eq 0 ] || fail "origin mode: exit $rc"
[ "$(bare_main)" = "$(local_main)" ] || fail "origin mode: remote main differs"
git -C "$repo" rev-parse --abbrev-ref 'main@{upstream}' >/dev/null 2>&1 || fail "origin mode: upstream not set"
expect_actions "$(local_main)"
pass "origin mode pushes and sets the upstream"

# 10. local main moves during fast: refused after yes, remote untouched
new_case "$fake_https"
extra_env=(STUB_JUST_COMMIT=1)
orig_main="$(local_main)"
run_push $'yes\n' --remote "$fake_https"
expect_refused "moved"
expect_one_fast
! grep -q 'moved-during-fast' "$out" || fail "moved main: the listing includes the commit made during fast"
grep -q "commit(s) up to $orig_main " "$out" || fail "moved main: the listing is not built from the checked sha $orig_main"
grep -q ': 2 commit(s) up to ' "$out" || fail "moved main: the count is not that of the checked sha"
[ -z "$(bare_main)" ] || fail "moved main: something was pushed"
expect_no_actions
pass "local main moving during fast refused after yes, remote unchanged"

# 11. the remote advances divergently during fast: the push is rejected, never forced
new_case "$fake_https"
new_other
extra_env=("STUB_JUST_ADVANCE=$c/other")
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -ne 0 ] || fail "divergent advance: push.sh exited 0"
expect_one_fast
[ "$(bare_main)" = "$(git -C "$c/other" rev-parse main)" ] || fail "divergent advance: remote main was overwritten"
expect_no_actions
grep -q 'refused: ' "$out" || fail "divergent advance: no refusal printed"
pass "remote advancing divergently during fast: push rejected, remote main kept, no Actions line"

# 12. unreachable remote refused before fast
new_case
run_push $'yes\n' --remote "file://$c/missing.git"
expect_refused "cannot reach the remote"
expect_no_fast
pass "unreachable remote refused before fast"

# 13. push-denied remote (reads work, push URL is dead) refused before fast
new_case "$fake_https"
git -C "$repo" config "url.file://$c/nowhere.git.pushInsteadOf" "$fake_https"
run_push $'yes\n' --remote "$fake_https"
expect_refused "no push access"
expect_no_fast
[ -z "$(bare_main)" ] || fail "push denied: something was pushed"
pass "push-denied remote refused by the dry run before fast"

# 14. replace refs never change what is listed, counted or checked for ancestry
fake_commit() { # makes a parentless commit and prints its sha
  git -C "$repo" commit-tree -m REPLACED "$(git -C "$repo" mktree </dev/null)"
}
new_case "$fake_https"
git -C "$repo" commit -q --allow-empty -m three
git -C "$repo" replace "$(git -C "$repo" rev-parse main~1)" "$(fake_commit)"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "replace listing: exit $rc"
grep -q ': 3 commit(s) up to ' "$out" || fail "replace listing: count follows the replace ref"
! grep -q 'REPLACED' "$out" || fail "replace listing: the listing shows the replacement"
grep -qE '^[0-9a-f]{40} [A-Za-z?] two$' "$out" || fail "replace listing: the real commit 'two' is not listed"
new_case "$fake_https"
git -C "$repo" push -q "file://$bare" "$(git -C "$repo" rev-parse main~1):refs/heads/main"
git -C "$repo" replace "$(local_main)" "$(fake_commit)"
run_push $'yes\n' --remote "$fake_https"
[ "$rc" -eq 0 ] || fail "replace ancestry: exit $rc, ancestry follows the replace ref"
[ "$(bare_main)" = "$(local_main)" ] || fail "replace ancestry: remote main differs"
pass "replace refs change neither the listing, the count nor the ancestry check"

# 15. the listing never starts a pager, even on a terminal (needs script(1) and a pty)
new_case "$fake_https"
marker="$c/pager-ran"
if script -qec true /dev/null >/dev/null 2>&1; then
  cmd="$(printf '%q ' "$bashbin" "$push" --remote "$fake_https")"
  rc=0
  (cd "$repo" && printf 'yes\n' | env PATH="$push_path" "GIT_PAGER=touch '$marker'; cat" script -qec "$cmd" /dev/null) >"$out" 2>&1 || rc=$?
  [ "$rc" -eq 0 ] || fail "pager: exit $rc"
  [ ! -e "$marker" ] || fail "pager: the listing started the pager"
  [ "$(bare_main)" = "$(local_main)" ] || fail "pager: remote main differs"
  pass "no pager under a pty"
else
  echo "founder-push-test: skipped: pager case (script(1) missing or no pty available)"
fi

echo "founder-push-test: ok ($passed groups)"
