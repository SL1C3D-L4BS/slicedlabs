#!/bin/bash
# Founder push of main to origin (WO-115, WO-119). Run from a normal terminal inside the repository:
#   bash scripts/founder/push.sh
# Preflight (nothing is pushed unless every step passes): just is installed; on branch main; tree clean,
# no untracked files; origin exists and is reachable; the remote main, if any, is an ancestor of local main;
# a dry-run push proves push access; `just fast` exits 0. Then it lists the commits to push, asks once for
# the word `yes`, and pushes the exact commit it checked to refs/heads/main: never forced, never tags.
# `--remote <url>` replaces origin (used by tests/gates/founder_push_test.sh, not by the founder).
set -euo pipefail

usage() {
  echo "usage: push.sh [--remote <url>]" >&2
  exit 2
}

refuse() {
  echo "push.sh: refused: $*" >&2
  exit 1
}

remote_url_arg=""
case "$#" in
  0) ;;
  2)
    [ "$1" = "--remote" ] || usage
    remote_url_arg="$2"
    [ -n "$remote_url_arg" ] || usage
    ;;
  *) usage ;;
esac

# Checks page of a GitHub remote URL (https, scp-like ssh or ssh://) for the pushed sha; empty for any other host.
actions_url() {
  local u="$1" sha="$2" host path
  u="${u%/}"
  u="${u%.git}"
  if [[ "$u" =~ ^https?://([^@/]+@)?([^/:]+)(:[0-9]+)?/(.+)$ ]]; then
    host="${BASH_REMATCH[2]}"
    path="${BASH_REMATCH[4]}"
  elif [[ "$u" =~ ^ssh://([^@/]+@)?([^/:]+)(:[0-9]+)?/(.+)$ ]]; then
    host="${BASH_REMATCH[2]}"
    path="${BASH_REMATCH[4]}"
  elif [[ "$u" =~ ^([^@/:]+@)?([^/:]+):(.+)$ ]]; then
    host="${BASH_REMATCH[2]}"
    path="${BASH_REMATCH[3]}"
  else
    return 0
  fi
  [ "$host" = "github.com" ] || return 0
  printf 'https://github.com/%s/commit/%s/checks\n' "$path" "$sha"
}

command -v just >/dev/null 2>&1 || refuse "just is not installed or not on PATH"
git rev-parse --git-dir >/dev/null || refuse "not a usable git repository (see git's message above)"
top="$(git rev-parse --show-toplevel)" || refuse "cannot find the repository top level (see git's message above)"
cd "$top"

# 1. branch
branch="$(git symbolic-ref --quiet --short HEAD || true)"
[ "$branch" = "main" ] || refuse "not on branch main (on '${branch:-detached HEAD}')"

# 2. clean tree, untracked files included
if [ -n "$(git status --porcelain --untracked-files=all)" ]; then
  git status --short >&2
  refuse "working tree is not clean (modified or untracked files)"
fi

# 3. remote exists
if [ -n "$remote_url_arg" ]; then
  target="$remote_url_arg"
  display_url="$remote_url_arg"
else
  git remote get-url origin >/dev/null 2>&1 || refuse "no remote named origin"
  target="origin"
  display_url="$(git config --get remote.origin.url)"
fi

# 4. reachable
ls_out="$(git ls-remote "$target" refs/heads/main 2>&1)" || {
  echo "$ls_out" >&2
  refuse "cannot reach the remote"
}
remote_main="$(printf '%s\n' "$ls_out" | awk '$2 == "refs/heads/main" {print $1}')"

# 5. remote main must be an ancestor of local main (the exact commit that is listed and pushed)
local_main="$(git rev-parse --verify refs/heads/main)"
if [ -n "$remote_main" ]; then
  if ! git cat-file -e "$remote_main^{commit}" 2>/dev/null; then
    refuse "remote main $remote_main is not in this clone; fetch from origin, then retry"
  fi
  git merge-base --is-ancestor "$remote_main" "$local_main" ||
    refuse "remote main ($remote_main) is not an ancestor of local main; never forcing, never rewriting"
  if [ "$remote_main" = "$local_main" ]; then
    echo "push.sh: remote main already equals local main; nothing to push"
    exit 0
  fi
fi

# 6. push access, proven by a dry run before the long fast gate
refspec="$local_main:refs/heads/main"
push_args=(push --no-follow-tags)
git "${push_args[@]}" --dry-run "$target" "$refspec" >/dev/null 2>&1 ||
  refuse "no push access: a dry-run push to the remote failed"

# 7. the fast gate is green (stdin closed so it cannot swallow the answer)
just fast </dev/null || refuse "just fast is red"

# list and ask
if [ -n "$remote_main" ]; then
  range="$remote_main..$local_main"
else
  range="$local_main"
fi
GIT_NO_REPLACE_OBJECTS=1 git --no-pager -c core.abbrev=40 log --no-decorate --format='%H %G? %s' "$range"
count="$(git rev-list --count "$range")"
echo "push.sh: $count commit(s) up to $local_main to push to $display_url (main only, no tags, no force)"
printf 'Type yes to push: '
answer=""
IFS= read -r answer || true
answer="${answer%$'\r'}"
[ "$answer" = "yes" ] || refuse "answer was not 'yes'; nothing pushed"

# main must still be the commit that was checked and listed
now_main="$(git rev-parse --verify -q refs/heads/main || true)"
[ "$now_main" = "$local_main" ] || refuse "local main moved from $local_main to ${now_main:-nothing} while waiting; nothing pushed"

# push
git "${push_args[@]}" "$target" "$refspec" || refuse "the push failed or was rejected; nothing was forced"
if [ -z "$remote_url_arg" ] && ! git rev-parse --abbrev-ref --symbolic-full-name 'main@{upstream}' >/dev/null 2>&1; then
  git config branch.main.remote origin
  git config branch.main.merge refs/heads/main
fi

url="$(actions_url "$display_url" "$local_main")"
if [ -n "$url" ]; then
  echo "Actions: $url"
else
  echo "push.sh: pushed $local_main; no Actions URL (not a github.com remote)"
fi
