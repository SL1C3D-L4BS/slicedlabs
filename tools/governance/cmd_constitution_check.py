"""constitution-check: SL-GOV-001 and SL-GOV-006 `sl-data` blocks agree with the master plan.

Rules (each failure line contains the rule id):
  R-CONST-LOCKED  the 19 locked decisions of SL-GOV-001 equal the master plan section 1.5 table (area and
                  decision text, whitespace-normalised, in order); each rejected item names a locked row that
                  contains it; `conditional` is a list
  R-CONST-FRAG    the F1-F8 rows of SL-GOV-006 equal the master plan section 3 table (id, invariant, numerator,
                  denominator, threshold, enforcement, episodes); the exemption classes equal section 3.1 and
                  the scorecard items equal section 3.3

The documents hold data only in fenced blocks with the info string sl-data (one JSON value each); prose is never
parsed. Stdlib only (ADR-0004); run with `python3 -I cmd_constitution_check.py [--root DIR]`.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

PLAN_REL = "docs/plan/SL-PLAN-001-master-plan.md"
CONSTITUTION_REL = "docs/gov/SL-GOV-001-constitution.md"
BUDGET_REL = "docs/gov/SL-GOV-006-fragmentation-budget.md"
LOCKED_COUNT = 19
FRAG_COUNT = 8

_FENCE = re.compile(r"^\s*```(\S*)\s*$")
_CELL_SPLIT = re.compile(r"(?<!\\)\|")
_HEADING = re.compile(r"^#{2,3}\s+\**(.*?)\**\s*$")


def norm(text):
    return re.sub(r"\s+", " ", text).strip()


def unescape(text):
    """Drop markdown backslash escapes (`\\+`, `\\)`, `\\>`), then normalise whitespace."""
    return norm(re.sub(r"\\([^\w\s])", r"\1", text))


def sl_data_blocks(text):
    """Return (blocks, errors): every `sl-data` fenced block parsed as JSON, with errors for bad JSON."""
    blocks, errors = [], []
    info, start, buf = None, 0, []
    for n, line in enumerate(text.splitlines(), start=1):
        m = _FENCE.match(line)
        if info is None:
            if m and m.group(1):
                info, start, buf = m.group(1), n, []
            elif m:
                info, start, buf = "", n, []
            continue
        if m and not m.group(1):
            if info == "sl-data":
                try:
                    blocks.append(json.loads("\n".join(buf)))
                except ValueError as exc:
                    errors.append("line %d: sl-data block is not JSON: %s" % (start, exc))
            info = None
        else:
            buf.append(line)
    return blocks, errors


def _section(plan_text, number):
    """Lines of the plan section whose heading starts with `number` (up to the next heading)."""
    lines = plan_text.splitlines()
    for i, line in enumerate(lines):
        m = _HEADING.match(line)
        if m and (m.group(1).startswith(number + " ") or m.group(1).startswith(number + "\\.")):
            out = []
            for later in lines[i + 1 :]:
                if later.startswith("#"):
                    break
                out.append(later)
            return out
    return None


def _table_rows(lines):
    rows = []
    for line in lines:
        s = line.strip()
        if not s.startswith("|"):
            continue
        cells = [unescape(c) for c in _CELL_SPLIT.split(s.strip("|"))]
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows[1:] if rows else rows  # drop the header row


def plan_locked(plan_text):
    """The section 1.5 table as [(area, decision)]. Raises ValueError when absent or malformed."""
    lines = _section(plan_text, "1.5")
    if lines is None:
        raise ValueError("%s: section 1.5 not found" % PLAN_REL)
    rows = _table_rows(lines)
    if any(len(r) != 2 for r in rows):
        raise ValueError("%s: section 1.5 table rows must have 2 cells" % PLAN_REL)
    return [(r[0], r[1]) for r in rows]


def plan_fragmentation(plan_text):
    """The section 3 table as a list of dicts shaped like the SL-GOV-006 rows."""
    lines = None
    for i, line in enumerate(plan_text.splitlines()):
        if re.match(r"^##\s+\**3\\?\.\s+Fragmentation budget", line):
            lines = []
            for later in plan_text.splitlines()[i + 1 :]:
                if later.startswith("#"):
                    break
                lines.append(later)
            break
    if lines is None:
        raise ValueError("%s: section 3 not found" % PLAN_REL)
    out = []
    for r in _table_rows(lines):
        if len(r) != 6:
            raise ValueError("%s: section 3 table rows must have 6 cells" % PLAN_REL)
        numerator, _, denominator = r[2].partition(" / ")
        out.append(
            {
                "id": r[0],
                "invariant": r[1],
                "numerator": numerator.strip(),
                "denominator": denominator.strip(),
                "threshold": r[3],
                "enforcement": r[4],
                "episodes": ["SL-EP-%03d" % int(n) for n in re.findall(r"\d+", r[5])],
            }
        )
    return out


def plan_exemptions(plan_text):
    lines = _section(plan_text, "3.1")
    if lines is None:
        raise ValueError("%s: section 3.1 not found" % PLAN_REL)
    m = re.search(r"Allowed classes:\s*(.+?)\.\s*$", unescape(" ".join(lines)))
    if not m:
        raise ValueError("%s: section 3.1 has no 'Allowed classes:' sentence" % PLAN_REL)
    return [re.sub(r"^and\s+", "", p.strip()) for p in m.group(1).split(",")]


def plan_scorecard(plan_text):
    lines = _section(plan_text, "3.3")
    if lines is None:
        raise ValueError("%s: section 3.3 not found" % PLAN_REL)
    return [unescape(l.strip()[1:]).rstrip(".") for l in lines if re.match(r"^\s*[*-]\s+", l)]


def _read(root, rel):
    try:
        with open(os.path.join(root, *rel.split("/")), encoding="utf-8") as fh:
            return fh.read(), None
    except (OSError, UnicodeDecodeError) as exc:
        return None, "%s: %s" % (rel, exc)


def _block_with(text, rel, rule, key):
    """The first sl-data object holding `key`, or (None, errors)."""
    blocks, errors = sl_data_blocks(text)
    errors = ["%s: %s: %s" % (rel, rule, e) for e in errors]
    for b in blocks:
        if isinstance(b, dict) and key in b:
            return b, errors
    return None, errors + ["%s: %s: no sl-data block with key '%s'" % (rel, rule, key)]


def locked_errors(doc_text, plan_text, rel=CONSTITUTION_REL):
    rule = "R-CONST-LOCKED"
    try:
        expected = plan_locked(plan_text)
    except ValueError as exc:
        return ["%s: %s" % (rule, exc)]
    errors = []
    if len(expected) != LOCKED_COUNT:
        errors.append(
            "%s: %s: master plan section 1.5 has %d rows, expected %d" % (PLAN_REL, rule, len(expected), LOCKED_COUNT)
        )
    block, errs = _block_with(doc_text, rel, rule, "locked")
    errors += errs
    if block is None:
        return errors
    locked = block["locked"]
    if not isinstance(locked, list) or not all(
        isinstance(r, dict) and isinstance(r.get("area"), str) and isinstance(r.get("decision"), str) for r in locked
    ):
        return errors + ["%s: %s: 'locked' must be a list of {area, decision} strings" % (rel, rule)]
    if len(locked) != LOCKED_COUNT:
        errors.append("%s: %s: %d locked rows, expected %d" % (rel, rule, len(locked), LOCKED_COUNT))
    have = [(norm(r["area"]), norm(r["decision"])) for r in locked]
    for area, decision in expected:
        if (area, decision) in have:
            continue
        same_area = [d for a, d in have if a == area]
        if same_area:
            errors.append(
                "%s: %s: row '%s' differs from the plan: have %r, plan %r" % (rel, rule, area, same_area[0], decision)
            )
        else:
            errors.append("%s: %s: plan row '%s' is missing" % (rel, rule, area))
    plan_areas = [a for a, _ in expected]
    for area, _ in have:
        if area not in plan_areas:
            errors.append("%s: %s: row '%s' is not in master plan section 1.5" % (rel, rule, area))
    if not errors and [a for a, _ in have] != plan_areas:
        errors.append("%s: %s: rows are not in the plan's order" % (rel, rule))
    rejected = block.get("rejected")
    if not isinstance(rejected, list):
        errors.append("%s: %s: 'rejected' must be a list of {item, source_row}" % (rel, rule))
    else:
        by_area = dict(have)
        for r in rejected:
            if not (isinstance(r, dict) and isinstance(r.get("item"), str) and isinstance(r.get("source_row"), str)):
                errors.append("%s: %s: rejected entries must be {item, source_row} strings" % (rel, rule))
            elif r["source_row"] not in by_area:
                errors.append(
                    "%s: %s: rejected '%s': source_row '%s' is not a locked row"
                    % (rel, rule, r["item"], r["source_row"])
                )
            elif norm(r["item"]).lower() not in by_area[r["source_row"]].lower():
                errors.append(
                    "%s: %s: rejected '%s' does not appear in row '%s'" % (rel, rule, r["item"], r["source_row"])
                )
    if not isinstance(block.get("conditional"), list):
        errors.append("%s: %s: 'conditional' must be a list" % (rel, rule))
    return errors


def frag_errors(doc_text, plan_text, rel=BUDGET_REL):
    rule = "R-CONST-FRAG"
    try:
        expected = plan_fragmentation(plan_text)
        exemptions = plan_exemptions(plan_text)
        scorecard = plan_scorecard(plan_text)
    except ValueError as exc:
        return ["%s: %s" % (rule, exc)]
    errors = []
    if len(expected) != FRAG_COUNT or [r["id"] for r in expected] != ["F%d" % n for n in range(1, FRAG_COUNT + 1)]:
        errors.append("%s: %s: master plan section 3 table is not F1..F8" % (PLAN_REL, rule))
    block, errs = _block_with(doc_text, rel, rule, "fragmentation")
    errors += errs
    if block is None:
        return errors
    rows = block["fragmentation"]
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        return errors + ["%s: %s: 'fragmentation' must be a list of objects" % (rel, rule)]
    by_id = {r.get("id"): r for r in rows}
    if len(by_id) != len(rows):
        errors.append("%s: %s: duplicate ids in 'fragmentation'" % (rel, rule))
    if len(rows) != len(expected):
        errors.append("%s: %s: %d rows, expected %d" % (rel, rule, len(rows), len(expected)))
    for want in expected:
        have = by_id.get(want["id"])
        if have is None:
            errors.append("%s: %s: %s is missing" % (rel, rule, want["id"]))
            continue
        for key in ("invariant", "numerator", "denominator", "threshold", "enforcement"):
            if not isinstance(have.get(key), str) or norm(have[key]) != want[key]:
                errors.append(
                    "%s: %s: %s.%s is %r, plan has %r" % (rel, rule, want["id"], key, have.get(key), want[key])
                )
        if have.get("episodes") != want["episodes"]:
            errors.append(
                "%s: %s: %s.episodes is %r, plan has %r"
                % (rel, rule, want["id"], have.get("episodes"), want["episodes"])
            )
    for r in rows:
        if r.get("id") not in [w["id"] for w in expected]:
            errors.append("%s: %s: %r is not an F-id of the plan" % (rel, rule, r.get("id")))
    if block.get("exemptions") != exemptions:
        errors.append(
            "%s: %s: 'exemptions' is %r, master plan section 3.1 has %r"
            % (rel, rule, block.get("exemptions"), exemptions)
        )
    if block.get("scorecard") != scorecard:
        errors.append(
            "%s: %s: 'scorecard' differs from master plan section 3.3 (%d items)" % (rel, rule, len(scorecard))
        )
    flow = block.get("flow")
    if not isinstance(flow, list) or not flow:
        errors.append("%s: %s: 'flow' (master plan section 3.2) must be a non-empty list" % (rel, rule))
    return errors


def check_all(root):
    plan, err = _read(root, PLAN_REL)
    if err:
        return ["R-CONST-LOCKED: " + err, "R-CONST-FRAG: " + err]
    errors = []
    for rel, fn in ((CONSTITUTION_REL, locked_errors), (BUDGET_REL, frag_errors)):
        text, err = _read(root, rel)
        if err:
            rule = "R-CONST-LOCKED" if fn is locked_errors else "R-CONST-FRAG"
            errors.append("%s: %s" % (rule, err))
        else:
            errors += fn(text, plan, rel)
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_constitution_check.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("constitution-check", check_all(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
