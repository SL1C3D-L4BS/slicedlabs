"""ep-advance <id> <state> <wo> [--adr ADR-nnnn] [--date YYYY-MM-DD] [--root DIR]: move one episode along the
master plan 6.2 lifecycle in registry/SL-REG-002-episodes.json and append the history entry.

Checks before it writes (any failure leaves the registry untouched, exit 1):
  - the episode exists and `status -> state` is a legal 6.2 transition (slib.TRANSITIONS)
  - work-orders/<wo>.md exists and its `Traces:` line names the episode; the WO is added to the episode's
    work_orders when missing
  - Demonstrated and Accepted need evidence/<wo>.json with pass true, wo == <wo> and
    wo_sha256 == sha256(work-orders/<wo>.md)  (R-NO-EVIDENCE); the episode's completion record gets the
    evidence path
  - entering InProgress, Verifying, Demonstrated or Accepted needs every dependency Accepted (R-DEP-ACCEPTED),
    unless --adr names the registry waiver (lifecycle.waivers) that covers the episode and the target state
  - the rule engine reports no finding that the registry did not already have

Exit 0 advanced, 1 refused, 2 usage or unreadable input. Stdlib only (ADR-0004), no subprocess, no network.
Run with `python3 -I cmd_ep_advance.py`.
"""

import datetime
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_wo_check  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

REGISTRY = "registry/SL-REG-002-episodes.json"
EP_RE = re.compile(r"^SL-EP-\d{3}$")
WO_RE = re.compile(r"^WO-\d{3}$")
ADR_RE = re.compile(r"^ADR-\d{4}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


class Refused(Exception):
    """The advance is not allowed; the message says why."""


def sha256_file(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def load_evidence(root, wo):
    """The parsed evidence/<wo>.json, or Refused (R-NO-EVIDENCE)."""
    rel = "evidence/%s.json" % wo
    wo_path = os.path.join(root, "work-orders", wo + ".md")
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        raise Refused("R-NO-EVIDENCE: %s does not exist (run scripts/evidence-record %s)" % (rel, wo))
    try:
        data = slib.load_json(path)
    except (OSError, ValueError) as exc:
        raise Refused("R-NO-EVIDENCE: %s is unreadable: %s" % (rel, exc))
    if not isinstance(data, dict):
        raise Refused("R-NO-EVIDENCE: %s is not a JSON object" % rel)
    if data.get("wo") != wo:
        raise Refused("R-NO-EVIDENCE: %s records wo %r, not %s" % (rel, data.get("wo"), wo))
    if data.get("pass") is not True:
        raise Refused("R-NO-EVIDENCE: %s does not record pass true" % rel)
    recorded = data.get("wo_sha256")
    if not isinstance(recorded, str) or not SHA_RE.match(recorded):
        raise Refused("R-NO-EVIDENCE: %s has no valid wo_sha256" % rel)
    if recorded != sha256_file(wo_path):
        raise Refused("R-NO-EVIDENCE: %s wo_sha256 differs from sha256(work-orders/%s.md): the work order changed after the evidence was recorded" % (rel, wo))
    return data


def waiver_for(reg, ep, adr, state):
    """The registry waiver named by `adr` that covers ep up to `state`, or Refused."""
    for w in reg["lifecycle"]["waivers"]:
        if w["rule"] == "R-DEP-ACCEPTED" and w["adr"] == adr and w["first"] <= ep["number"] <= w["last"]:
            if slib.PROGRESS.index(state) <= slib.PROGRESS.index(w["max_status"]):
                return w
            raise Refused("R-DEP-ACCEPTED: %s waives only up to %s, not %s" % (adr, w["max_status"], state))
    raise Refused("R-DEP-ACCEPTED: the registry has no waiver %s covering %s" % (adr, ep["id"]))


def check_dependencies(reg, ep, state, adr):
    """Enforce R-DEP-ACCEPTED for the target state. Returns the waiver used (or None)."""
    waiver = waiver_for(reg, ep, adr, state) if adr else None
    if state not in slib.STARTED or waiver:
        return waiver
    index = {e["id"]: e for e in reg["episodes"]}
    pending = ["%s (%s)" % (d, index[d]["status"]) for d in ep["depends_on"] if d in index and index[d]["status"] != "Accepted"]
    if pending:
        raise Refused(
            "R-DEP-ACCEPTED: %s cannot be %s while dependencies are not Accepted: %s (waiver: pass --adr)"
            % (ep["id"], state, ", ".join(pending))
        )
    return None


def advance(reg, root, ep_id, state, wo, date, adr=None):
    """Mutate reg in place; return the history entry. Raises Refused. Reads work-orders/ and evidence/ under root."""
    if state not in slib.STATES:
        raise Refused("unknown state '%s' (states: %s)" % (state, ", ".join(slib.STATES)))
    ep = next((e for e in reg["episodes"] if e["id"] == ep_id), None)
    if ep is None:
        raise Refused("%s is not in the registry" % ep_id)
    old = ep["status"]
    if not slib.can_transition(old, state):
        nxt = ", ".join(slib.next_states(old)) or "none"
        raise Refused("R-LIFECYCLE: %s > %s is not a legal transition (master plan 6.2); from %s the next states are: %s" % (old, state, old, nxt))
    wo_path = os.path.join(root, "work-orders", wo + ".md")
    if not os.path.isfile(wo_path):
        raise Refused("work-orders/%s.md does not exist" % wo)
    with open(wo_path, encoding="utf-8") as fh:
        _, body = cmd_wo_check.parse_work_order(fh.read())
    traced, _ = cmd_wo_check.traced_ids(body)
    if ep_id not in traced:
        raise Refused("R-WO-TRACE: work-orders/%s.md does not trace %s" % (wo, ep_id))
    waiver = check_dependencies(reg, ep, state, adr)
    evidence = None
    if state in slib.EVIDENCE_STATES:
        evidence = load_evidence(root, wo)
    entry = {"from": old, "to": state, "date": date}
    note = wo + (" (waiver %s)" % adr if waiver else "")
    entry["note"] = note
    ep["history"].append(entry)
    ep["status"] = state
    if wo not in ep["work_orders"]:
        ep["work_orders"] = sorted(ep["work_orders"] + [wo])
    if evidence is not None:
        rel = "evidence/%s.json" % wo
        comp = ep["completion"]
        if comp is None:
            comp = {"revision": str(evidence.get("merge_commit") or wo), "evidence": [], "benchmarks": [], "open_defects": [], "exceptions": []}
            ep["completion"] = comp
        if rel not in comp["evidence"]:
            comp["evidence"].append(rel)
    return entry


def dump_registry(reg):
    """The layout cmd_gen_episodes.py writes (indent 1, sorted keys), so an advance changes only its own lines."""
    return json.dumps(reg, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def run(root, ep_id, state, wo, date, adr):
    path = os.path.join(root, REGISTRY)
    if not os.path.isfile(path):
        print("ep-advance: %s does not exist yet (WO-004 generates it); nothing to advance" % REGISTRY, file=sys.stderr)
        return 1
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        reg = slib.loads_json(text, REGISTRY)
    except (OSError, ValueError) as exc:
        print("ep-advance: cannot read %s: %s" % (REGISTRY, exc), file=sys.stderr)
        return 2
    before = {(f["rule"], f["path"], f["message"]) for f in slib.check_registry(reg)}
    try:
        entry = advance(reg, root, ep_id, state, wo, date, adr)
    except Refused as exc:
        print("ep-advance: refused: %s" % exc, file=sys.stderr)
        return 1
    except (cmd_wo_check.WoParseError, slgov.FrontMatterError) as exc:
        print("ep-advance: refused: work-orders/%s.md does not parse: %s" % (wo, exc), file=sys.stderr)
        return 1
    new = [f for f in slib.check_registry(reg) if (f["rule"], f["path"], f["message"]) not in before]
    if new:
        for f in new:
            print("ep-advance: refused: %s" % slib.format_finding(f), file=sys.stderr)
        return 1
    out = dump_registry(reg)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(out)
    print("ep-advance: %s %s -> %s (%s, %s)" % (ep_id, entry["from"], entry["to"], entry["date"], entry["note"]))
    if dump_registry(slib.loads_json(text, REGISTRY)) != text:
        print("ep-advance: note: the registry file was re-laid out (indent 1, sorted keys); it differed from that layout before")
    return 0


def parse_args(argv):
    """(ep_id, state, wo, date, adr, root) or an error string."""
    args = argv[1:]
    pos, opts, i = [], {}, 0
    while i < len(args):
        a = args[i]
        if a in ("--adr", "--date", "--root"):
            if i + 1 >= len(args):
                return "%s needs a value" % a
            opts[a] = args[i + 1]
            i += 2
        elif a.startswith("--"):
            return "unknown option %s" % a
        else:
            pos.append(a)
            i += 1
    if len(pos) != 3:
        return "usage: cmd_ep_advance.py <SL-EP-nnn> <state> <WO-nnn> [--adr ADR-nnnn] [--date YYYY-MM-DD]"
    ep_id, state, wo = pos
    if not EP_RE.match(ep_id):
        return "'%s' is not an SL-EP-nnn id" % ep_id
    if not WO_RE.match(wo):
        return "'%s' is not a WO-nnn id" % wo
    adr = opts.get("--adr")
    if adr is not None and not ADR_RE.match(adr):
        return "'%s' is not an ADR-nnnn id" % adr
    date = opts.get("--date") or datetime.date.today().isoformat()
    if not DATE_RE.match(date):
        return "'%s' is not a YYYY-MM-DD date" % date
    try:
        datetime.date.fromisoformat(date)
    except ValueError:
        return "'%s' is not a real date" % date
    return ep_id, state, wo, date, adr, os.path.abspath(opts.get("--root") or slgov.ROOT)


def main(argv):
    parsed = parse_args(argv)
    if isinstance(parsed, str):
        print(parsed, file=sys.stderr)
        return 2
    ep_id, state, wo, date, adr, root = parsed
    return run(root, ep_id, state, wo, date, adr)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
