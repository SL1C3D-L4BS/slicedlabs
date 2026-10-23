#!/usr/bin/env bash
# approve-001-080 (WO-069, WO-074, WO-112): the founder's one-shot close-out for every episode in the map (001-100 today). FOUNDER-RUN ONLY; agents never run it.
#
# Usage: scripts/founder/approve-001-080.sh [--jobs-dir DIR] [--no-verify] [--date YYYY-MM-DD] [--yes] [--no-sign]
#
# In order, stopping at the first failure (set -e):
#   0 preflight   main branch, clean tree (evidence/ may be untracked), registry-check, adr-check, wo-check; a summary;
#                 one "yes" from the terminal (or --yes)
#   1 evidence    1a untracked evidence/*.json is committed first (wo-verify refuses a tree with untracked files);
#                 1b for every work order in build/closeout/ep-map.txt without evidence/WO-NNN.json: wo-verify in the
#                 jail (fast gate + the order's acceptance; --jobs-dir, default build/jobs), unless the jobs evidence
#                 already says pass true for the current work-order hash; a refused or red run STOPS the script and
#                 is never recorded (--no-verify never runs wo-verify and requires passing jobs evidence);
#                 1c scripts/evidence-record --direct for each; 1d evidence-verify
#   2 ADRs        ADR-0001..ADR-0021 status Proposed -> Accepted, the ADR log regenerated, adr-check
#   3 SL-GOV-001  front matter status Approved, its sha256 and version into registry/SL-REG-001.approvals.json,
#                 SL-REG-001 regenerated, registry-docs-check, constitution-frozen, doc-check
#   4 episodes    every row of ep-map.txt in file order: 001-020 to Accepted, 041-080 to Demonstrated, one
#                 ep-advance per lifecycle step (Specified, InProgress, Verifying, Demonstrated[, Accepted]), with
#                 --adr <waiver> on started states when the registry holds a waiver covering the episode (ADR-0003 for
#                 041-048; ADR-0020 for 061-080 once its cap is in the registry); then registry-views, registry-check
#   5 commit      evidence/, registry/, docs/adr/, SL-GOV-001 and the ADR log, explicit pathspecs, `git commit -S`
#                 (--no-sign is for the throwaway-clone test only)
# Re-runnable: evidence that exists is kept, an ADR already Accepted is left alone, an episode resumes from its
# current state. Never pushes, never fetches. Expect hours when step 1 has to verify Volume 04 (WO-063's acceptance
# runs check-full).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
jobs_dir="$root/build/jobs"
no_verify=0
no_sign=0
auto_yes=0
date_val="$(date +%F)"
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs-dir)
      jobs_dir="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
      shift
      ;;
    --no-verify) no_verify=1 ;;
    --no-sign) no_sign=1 ;;
    --yes) auto_yes=1 ;;
    --date)
      date_val="$2"
      shift
      ;;
    -h | --help)
      sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 2
      ;;
    *)
      echo "approve-001-080: unknown argument $1" >&2
      exit 2
      ;;
  esac
  shift
done
[[ "$date_val" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || {
  echo "approve-001-080: --date must be YYYY-MM-DD" >&2
  exit 2
}

die() {
  echo "approve-001-080: FAIL: $*" >&2
  exit 1
}
step() { printf '\n== %s\n' "$*"; }
run() {
  printf '   $ %s\n' "$*"
  "$@"
}

ep_map="build/closeout/ep-map.txt"
wo_verify="$HOME/.dotfiles/tools/factory/wo-verify"
adrs=()
for f in docs/adr/ADR-[0-9][0-9][0-9][0-9]-*.md; do adrs+=("$(basename "$f" | cut -c1-8)"); done # every ADR on disk, in id order

# ---- 0 preflight -------------------------------------------------------------------------------------------------
step "0 preflight"
[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || die "not on main"
dirty="$(git status --porcelain | grep -v '^?? evidence/' || true)"
[ -z "$dirty" ] || die "the tree is not clean (only untracked evidence/ is allowed):"$'\n'"$dirty"
[ -f "$ep_map" ] || die "$ep_map is missing"
bad_evidence="$(for f in evidence/WO-*.json; do
  [ -f "$f" ] || continue
  python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d.get("pass") is True else 1)' "$f" || printf '%s ' "$f"
done)"
[ -z "$bad_evidence" ] || die "evidence with pass=false exists (a refused run was recorded); remove it and its job, then re-run:"$'\n'"  rm -f $bad_evidence; rm -rf $(for f in $bad_evidence; do printf '%s/%s ' "$jobs_dir" "$(basename "$f" .json)"; done)"
for a in "${adrs[@]}"; do
  ls docs/adr/"$a"-*.md >/dev/null 2>&1 || die "docs/adr/$a-*.md does not exist"
done
[ "$no_verify" -eq 1 ] || [ -x "$wo_verify" ] || die "$wo_verify is not executable (or pass --no-verify)"
run just registry-check
run just adr-check
run just wo-check

mapfile -t rows < <(grep -vE '^\s*(#|$)' "$ep_map")
missing=()
declare -A seen=()
for row in "${rows[@]}"; do
  wo="$(awk '{print $2}' <<<"$row")"
  [[ "$wo" =~ ^WO-[0-9]{3}$ ]] || die "bad ep-map row: $row"
  [ -f "work-orders/$wo.md" ] || die "work-orders/$wo.md does not exist (ep-map row: $row)"
  if [ -z "${seen[$wo]:-}" ]; then
    seen[$wo]=1
    [ -f "evidence/$wo.json" ] || missing+=("$wo")
  fi
done
to_accept=0
to_demo=0
for row in "${rows[@]}"; do
  n="$(awk '{print $1}' <<<"$row")"
  if [ "$((10#$n))" -le 20 ]; then to_accept=$((to_accept + 1)); else to_demo=$((to_demo + 1)); fi
done
gov_sha="$(sha256sum docs/gov/SL-GOV-001-constitution.md | cut -c1-64)"
gov_version="$(sed -n 's/^version: //p' docs/gov/SL-GOV-001-constitution.md | head -n 1)"
cat <<SUMMARY

approve-001-080 will, at HEAD $(git rev-parse --short HEAD) on $date_val:
  1 record evidence for ${#missing[@]} work order(s) without evidence/: ${missing[*]:-none}
    $([ "$no_verify" -eq 1 ] && echo "using existing jobs evidence in $jobs_dir (--no-verify)" || echo "running wo-verify in the jail for each (jobs dir $jobs_dir; hours for Volume 04)")
  2 set ${#adrs[@]} ADRs (${adrs[0]}..${adrs[-1]}) to Accepted and regenerate docs/gov/SL-GOV-004-adr-log.md
  3 approve SL-GOV-001 (version $gov_version, sha256 ${gov_sha:0:16}...) and freeze the Constitution
  4 advance $to_accept episode(s) to Accepted and $to_demo to Demonstrated (${#rows[@]} rows in $ep_map)
  5 commit evidence/, registry/, docs/adr/, SL-GOV-001 and the ADR log $([ "$no_sign" -eq 1 ] && echo "UNSIGNED (--no-sign, test only)" || echo "signed (git commit -S)")
It never pushes. Every step stops the script on its first failure; re-running resumes.
SUMMARY
if [ "$auto_yes" -eq 1 ]; then
  echo "proceeding (--yes)"
else
  printf 'Type yes to proceed: '
  read -r answer
  [ "$answer" = "yes" ] || die "not confirmed"
fi

# ---- 1 evidence --------------------------------------------------------------------------------------------------
step "1 evidence (${#missing[@]} to record)"
mkdir -p "$jobs_dir"
# 1a: wo-verify refuses a tree with untracked files, so evidence recorded earlier must be committed first
if [ -n "$(git status --porcelain -- evidence)" ]; then
  run git add -- evidence
  if [ "$no_sign" -eq 1 ]; then
    run git -c commit.gpgsign=false commit -q -m "evidence: records already verified, committed before verification of the rest (approve-001-080 step 1a)"
  else
    run git commit -q -S -m "evidence: records already verified, committed before verification of the rest (approve-001-080 step 1a)"
  fi
fi
job_passed() { # <wo>: the jobs evidence says pass true for the current work-order hash
  python3 -I - "$jobs_dir/$1/evidence.json" "work-orders/$1.md" <<'PY'
import hashlib, json, sys
try:
    d = json.load(open(sys.argv[1]))
except (OSError, ValueError):
    sys.exit(1)
sha = hashlib.sha256(open(sys.argv[2], "rb").read()).hexdigest()
sys.exit(0 if d.get("pass") is True and d.get("wo_sha256") == sha else 1)
PY
}
# 1b: verify every missing order while the tree is clean (evidence is recorded only after all of them pass)
for wo in "${missing[@]}"; do
  if job_passed "$wo"; then
    echo "   $wo: jobs evidence already passed for this work-order hash"
    continue
  fi
  [ "$no_verify" -eq 0 ] || die "no passing jobs evidence at $jobs_dir/$wo/evidence.json and --no-verify was given"
  rm -rf "${jobs_dir:?}/$wo"
  vt=14400
  at=10800
  if grep -qE 'just (acceptance-[0-9]{3}-[0-9]{3}|check-full)' "work-orders/$wo.md"; then
    vt=36000 # a release-gate order runs check-full inside its acceptance (about 4 h on this machine)
    at=32400
  fi
  run python3 -I "$wo_verify" --wo "work-orders/$wo.md" --worktree . --base main --gate fast \
    --jobs-dir "$jobs_dir" --timeout "$vt" --ac-timeout "$at"
  job_passed "$wo" || die "$wo did not pass wo-verify; see $jobs_dir/$wo/evidence.json (reasons) and gate-tail.log; nothing recorded"
done
# 1c: record, 1d: verify
for wo in "${missing[@]}"; do
  FACTORY_JOBS_DIR="$jobs_dir" run bash scripts/evidence-record "$wo" --direct --date "$date_val"
done
run just evidence-verify

# ---- 2 ADRs ------------------------------------------------------------------------------------------------------
step "2 ADRs to Accepted"
for a in "${adrs[@]}"; do
  f="$(ls docs/adr/"$a"-*.md)"
  if grep -q '^status: Accepted$' "$f"; then
    echo "   $a already Accepted"
  elif grep -q '^status: Proposed$' "$f"; then
    sed -i '0,/^status: Proposed$/s//status: Accepted/' "$f"
    echo "   $a -> Accepted"
  else
    die "$f has neither status Proposed nor Accepted"
  fi
done
run python3 -I tools/governance/cmd_gen_adr_log.py
run just adr-check

# ---- 3 SL-GOV-001 ------------------------------------------------------------------------------------------------
step "3 SL-GOV-001 Approved and frozen"
gov=docs/gov/SL-GOV-001-constitution.md
if grep -q '^status: Draft$' "$gov"; then
  sed -i '0,/^status: Draft$/s//status: Approved/' "$gov"
fi
grep -q '^status: Approved$' "$gov" || die "$gov status is not Approved"
gov_sha="$(sha256sum "$gov" | cut -c1-64)"
python3 -I - "$gov_sha" "$gov_version" "$date_val" <<'PY'
import json, sys
p = "registry/SL-REG-001.approvals.json"
d = json.load(open(p))
d["SL-GOV-001"] = {"sha256": sys.argv[1], "version": sys.argv[2], "date": sys.argv[3], "approver": "founder"}
json.dump(d, open(p, "w"), indent=2, sort_keys=True)
open(p, "a").write("\n")
print("   approvals: SL-GOV-001 %s %s" % (sys.argv[2], sys.argv[1][:16]))
PY
run python3 -I tools/governance/cmd_gen_docs.py
run just registry-docs-check
run just constitution-frozen
run just doc-check

# ---- 4 episodes --------------------------------------------------------------------------------------------------
step "4 episodes"
waiver_for() { # <number> <state> -> the registry waiver ADR covering it, or ""
  python3 -I - "$1" "$2" <<'PY'
import json, sys
n, state = int(sys.argv[1]), sys.argv[2]
order = ["Planned", "Specified", "InProgress", "Verifying", "Demonstrated", "Accepted"]
reg = json.load(open("registry/SL-REG-002-episodes.json"))
for w in reg["lifecycle"]["waivers"]:
    if w["rule"] == "R-DEP-ACCEPTED" and w["first"] <= n <= w["last"] and order.index(state) <= order.index(w["max_status"]):
        print(w["adr"]); break
PY
}
status_of() {
  python3 -I -c 'import json,sys; print({e["id"]: e["status"] for e in json.load(open("registry/SL-REG-002-episodes.json"))["episodes"]}[sys.argv[1]])' "$1"
}
path=(Specified InProgress Verifying Demonstrated Accepted)
for row in "${rows[@]}"; do
  n="$(awk '{print $1}' <<<"$row")"
  wo="$(awk '{print $2}' <<<"$row")"
  ep="SL-EP-$n"
  if [ "$((10#$n))" -le 20 ]; then target=Accepted; else target=Demonstrated; fi
  cur="$(status_of "$ep")"
  if [ "$cur" = "$target" ]; then
    echo "   $ep already $target"
    continue
  fi
  started=0
  for st in "${path[@]}"; do
    [ "$st" = "$cur" ] && started=1 && continue
    [ "$cur" = "Planned" ] && started=1
    [ "$started" -eq 1 ] || continue
    flags=()
    case "$st" in InProgress | Verifying | Demonstrated | Accepted)
      w="$(waiver_for "$n" "$st")"
      [ -n "$w" ] && flags=(--adr "$w")
      ;;
    esac
    run just ep-advance "$ep" "$st" "$wo" "${flags[@]}" --date "$date_val"
    [ "$st" = "$target" ] && break
  done
done
run just registry-views
run just registry-check
run just registry-views-check
run just evidence-verify
run just fast

# ---- 5 commit ----------------------------------------------------------------------------------------------------
step "5 commit"
run git add -- evidence registry docs/adr docs/gov/SL-GOV-001-constitution.md docs/gov/SL-GOV-004-adr-log.md
sign=(-S)
[ "$no_sign" -eq 1 ] && sign=(-c commit.gpgsign=false)
msg="Founder close-out: ${adrs[0]}..${adrs[-1]} Accepted, SL-GOV-001 Approved and frozen, evidence recorded, mapped episodes advanced ($to_accept to Accepted, $to_demo to Demonstrated)."
if [ "$no_sign" -eq 1 ]; then
  run git -c commit.gpgsign=false commit -q -m "$msg"
else
  run git commit -q "${sign[@]}" -m "$msg"
fi
echo
echo "approve-001-080: done at $(git rev-parse --short HEAD). Next: just registry-stats; then the first push (founder)."
