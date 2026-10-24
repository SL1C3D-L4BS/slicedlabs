#!/bin/bash
# Founder push of main to origin (WO-115). Run from a normal terminal inside the repository:
#   bash scripts/founder/push.sh
# Preflight (nothing is pushed unless every step passes): on branch main; tree clean, no untracked files;
# origin exists and is reachable; the remote main, if any, is an ancestor of local main; `just fast` exits 0.
# Then it lists the commits to push, asks once for the word `yes`, and pushes main only: never forced,
# never tags. `--remote <url>` replaces origin (used by tests/gates/founder_push_test.sh, not by the founder).
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

# Actions page of a GitHub-style remote URL (https, scp-like ssh or ssh://), empty when not recognised.
actions_url() {
  local u="$1" host path
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
  printf 'https://%s/%s/actions\n' "$host" "$path"
}

git rev-parse --git-dir >/dev/null 2>&1 || refuse "not inside a git repository"
cd "$(git rev-parse --show-toplevel)"

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

# 5. remote main must be an ancestor of local main
local_main="$(git rev-parse refs/heads/main)"
if [ -n "$remote_main" ]; then
  # a remote main this clone does not hold cannot be an ancestor either: refused the same way (no fetch needed)
  git merge-base --is-ancestor "$remote_main" "$local_main" >/dev/null 2>&1 ||
    refuse "remote main ($remote_main) is not an ancestor of local main; never forcing, never rewriting"
  if [ "$remote_main" = "$local_main" ]; then
    echo "push.sh: remote main already equals local main; nothing to push"
    exit 0
  fi
fi

# 6. the fast gate is green
just fast || refuse "just fast is red"

# list and ask
if [ -n "$remote_main" ]; then
  range="$remote_main..main"
else
  range="main"
fi
git log --oneline "$range"
count="$(git rev-list --count "$range")"
echo "push.sh: $count commit(s) to push to $display_url (main only, no tags, no force)"
printf 'Type yes to push: '
answer=""
read -r answer || answer=""
[ "$answer" = "yes" ] || refuse "answer was not 'yes'; nothing pushed"

# push
push_args=(push --no-follow-tags)
if [ -z "$remote_url_arg" ] && ! git rev-parse --abbrev-ref --symbolic-full-name 'main@{upstream}' >/dev/null 2>&1; then
  push_args+=(-u)
fi
git "${push_args[@]}" "$target" main:refs/heads/main

url="$(actions_url "$display_url")"
if [ -n "$url" ]; then
  echo "Actions: $url"
else
  echo "push.sh: pushed; no Actions URL for $display_url"
fi
