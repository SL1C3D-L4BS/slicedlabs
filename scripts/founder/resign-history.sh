#!/usr/bin/env bash
# resign-history (WO-069): re-sign every commit on main with the founder's key. FOUNDER-RUN ONLY, BEFORE THE FIRST PUSH.
#
# WARNING: this REWRITES EVERY COMMIT HASH on main (git rebase --root --exec 'git commit --amend -S'). Anything that
# recorded a hash before this run (evidence/*.json merge_commit fields, notes, other clones) will point at hashes
# that no longer exist. Run it only before the first push and before the approval script, or accept that the
# evidence files then name pre-rewrite hashes. It refuses to run when a remote exists or the tree is not clean.
# Usage: scripts/founder/resign-history.sh [--yes]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
auto_yes=0
[ "${1:-}" = "--yes" ] && auto_yes=1
[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || {
  echo "resign-history: not on main" >&2
  exit 1
}
[ -z "$(git remote)" ] || {
  echo "resign-history: a remote exists; never rewrite published history" >&2
  exit 1
}
[ -z "$(git status --porcelain | grep -v '^?? evidence/' || true)" ] || {
  echo "resign-history: the tree is not clean" >&2
  exit 1
}
key="$(git config user.signingkey || true)"
[ -n "$key" ] || {
  echo "resign-history: git config user.signingkey is not set" >&2
  exit 1
}
count="$(git rev-list --count HEAD)"
unsigned="$(git log --format='%G?' | grep -vc '^G$' || true)"
echo "resign-history: $count commits on main, $unsigned not signed with a good signature; key ${key:0:16}"
echo "WARNING: every commit hash will change. Evidence files that name merge commits will become stale."
if [ "$auto_yes" -eq 0 ]; then
  printf 'Type yes to rewrite main: '
  read -r answer
  [ "$answer" = "yes" ] || {
    echo "resign-history: not confirmed" >&2
    exit 1
  }
fi
git tag -f "pre-resign-$(date +%Y%m%d%H%M%S)" HEAD >/dev/null
git rebase --root --committer-date-is-author-date --exec 'git commit --amend --no-edit -S' main
echo "resign-history: done; $(git log --format='%G?' | grep -c '^G$') of $(git rev-list --count HEAD) commits now carry a good signature"
echo "A pre-resign-* tag keeps the old tip; delete it when you are sure."
