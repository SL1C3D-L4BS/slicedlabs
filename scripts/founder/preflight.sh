#!/usr/bin/env bash
# Founder pre-flight for the Sliced Labs monorepo (plan F-0). Run from a NORMAL terminal, not from an agent:
#   bash scripts/founder/preflight.sh
# Every step is idempotent and prints what it does. Steps needing sudo ask for your password.
set -euo pipefail
repo=/home/founder/Projects/slicedlabs
dot=/home/founder/.dotfiles
cd "$repo"

step() { printf '\n== %s\n' "$*"; }

step "1. remove sandbox mount-target stubs (0-byte files left by the agent sandbox)"
for f in .bash_profile .bashrc .zshrc .zprofile .profile .gitconfig .gitmodules .idea .vscode .mcp.json .ripgreprc; do
  if [ -e "$f" ] && [ ! -s "$f" ] && [ ! -d "$f" ]; then rm -v -- "$f"; fi
done
for f in agents commands hooks launch.json loop.md output-styles routines scheduled_tasks.json settings.json settings.local.json skills workflows; do
  p=".claude/$f"
  if [ -e "$p" ] && [ ! -s "$p" ] && [ ! -d "$p" ]; then rm -v -- "$p"; fi
done
find . -path ./.git -prune -o -type f -size 0 -print | grep -v '^./build/' || true

step "2. install doctest (and vcpkg if you want the optional provider now)"
sudo pacman -Syu --needed doctest
# sudo pacman -Syu --needed vcpkg   # optional provider; leave commented to skip

step "3. deliver the .claude pieces agents cannot write"
mkdir -p .claude/agents .claude/rules
cp -v "$dot/templates/factory/monorepo/.claude/agents/cpp-reviewer.md" .claude/agents/cpp-reviewer.md
if [ ! -s .claude/settings.json ]; then
  cat >.claude/settings.json <<'JSON'
{
  "permissions": {
    "allow": [
      "Bash(just *)", "Bash(cmake *)", "Bash(ctest *)", "Bash(ninja *)",
      "Bash(python3 -I tools/governance/*)", "Bash(python3 -I tools/deps/*)", "Bash(python3 -I tools/presets/*)",
      "Bash(python3 -I -m unittest *)", "Bash(clang-format *)", "Bash(run-clang-tidy *)",
      "Bash(git status*)", "Bash(git diff*)", "Bash(git log*)", "Bash(git ls-files*)", "Bash(git add *)", "Bash(git commit *)"
    ],
    "deny": ["Bash(git push*)", "Bash(git rebase*)", "Bash(sudo*)", "Bash(pacman*)", "Bash(yay*)", "Bash(chezmoi*)", "Bash(hyprctl*)"]
  },
  "disableClaudeAiConnectors": true
}
JSON
  echo "wrote .claude/settings.json (review it; agents never edit it)"
else
  echo ".claude/settings.json exists; merge the allow rules above by hand if missing"
fi

step "4. job.json for the work orders that touch gate files (config-protect and wo-verify read it)"
for wo in WO-001 WO-014 WO-020; do
  d="$HOME/.local/state/factory/jobs/$wo"
  mkdir -p "$d"
  [ -f "$d/job.json" ] || printf '{"id":"%s","state":"building","touches_gates":true,"worktree":"%s"}\n' "$wo" "$repo" >"$d/job.json"
  echo "$d/job.json"
done

step "5. dotfiles: D-144 and the CLAUDE.md layout line (review the diff, then commit in ~/.dotfiles yourself)"
if ! grep -q '^| D-144 ' "$dot/docs/decisions.md"; then
  # shellcheck disable=SC2016  # the backticks are literal Markdown
  printf '| D-144 | 2026-10-17 | The Sliced Labs master plan (SL-PLAN-001, 840 episodes, engine + editor + web platform) supersedes D-126 (engine scope) and D-135 (monorepo layout). The monorepo `~/Projects/slicedlabs` is recreated fresh with the layout engine/ editor/ sdk/ tools/ tests/ docs/ third_party/ examples/ (repo ADR-0001); repo decisions live in its docs/adr/ from here on. | Founder, 2026-10-17: "here is what i want to build ... state of the art"; the plan at ~/.claude/plans/here-is-what-i-melodic-owl.md. |\n' >>"$dot/docs/decisions.md"
  echo "appended D-144 to $dot/docs/decisions.md"
fi
grep -n 'sliced-engine/' "$dot/dot_claude/CLAUDE.md" || true
echo "  -> replace that layout line with: slicedlabs monorepo (engine/, editor/, sdk/, tools/, tests/, docs/, third_party/, examples/)"
(cd "$dot" && git status --short docs/decisions.md dot_claude/CLAUDE.md)

step "6. commit the genesis and the current wave (signed with your key)"
git add -- FACTORY.md justfile .gitignore .gitattributes README.md docs/plan work-orders scripts/founder
git status --short | head -30
echo "  -> review, then: git commit -m 'WO-000: genesis of the Sliced Labs monorepo'"
echo "  -> per work order, add only its paths_allowed (listed in each work-orders/WO-NNN.md) and commit as 'WO-NNN: <goal>'"

step "7. smoke-test the factory jail on the genesis (after the commit)"
echo "  -> python3 -I $dot/tools/factory/wo-verify --help   # then run it on WO-001 once it is built; pass --timeout/--ac-timeout if the gate exceeds 900 s / 300 s"
echo "  -> never add the old GitHub remote before this slice's first push (worktree.sh prefers origin/main)"
