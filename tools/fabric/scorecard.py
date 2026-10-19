"""fabric-scorecard: the Volume 04 scorecard from the conformance run (WO-062; master plan section 3 F2 and F5).

Input: the scorecard input written by fabric_conformance_gate1_test (format sl-fabric-scorecard-input/1: types run
and failed, the F2 report, the F5 ledger delta of the conformance workload, release gate 1, commits, objects written,
validation passes, validators, validator invocations and changed records). Invariant coverage is computed here with
tools/fabric/contracts_check.py in strict mode over the repository: bound invariants over all invariants.

Output: build/fabric/scorecard.json (format sl-fabric-scorecard/1, or --out PATH):

  f2                     {numerator, denominator}: domains with more than one owner over all domains (threshold 0)
  f5                     {numerator, denominator, enabled}: store mutations bypassing the transaction manager over all
                         store mutations of the workload (threshold 0)
  invariant_coverage     {bound, total, ratio}: threshold 1.0
  gate1                  {numerator, denominator}: records without owner, stable id or revision (threshold 0)
  objects_per_commit     objects written over commits
  validator_invocations  {invocations, bound, changed_records, validators}: invocations, at most changed records times
                         validators (SL-ARCH-002 Measures)
  types                  {run, failed}
  failures               one line per failed threshold, naming F2, F5, coverage, gate 1, types or validators

Exit 1 when the F2 or F5 numerator is not 0 (or the F5 ledger is compiled out, so nothing was measured), coverage is
below 1.0, gate 1 is not 0, a type failed the suite or validator invocations exceed their bound; 2 on bad usage or an
unreadable input. Stdlib only, no subprocess (ADR-0004).

  python3 -I tools/fabric/scorecard.py <scorecard-input.json> [--out PATH] [--root DIR]
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import contracts_check  # noqa: E402

INPUT_FORMAT = "sl-fabric-scorecard-input/1"
OUTPUT_FORMAT = "sl-fabric-scorecard/1"
OUT_PATH = "build/fabric/scorecard.json"
USAGE = "usage: scorecard.py <scorecard-input.json> [--out PATH] [--root DIR]"


def _count(doc, *keys):
    """The non-negative integer at doc[keys...]; ValueError otherwise."""
    value = doc
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            raise ValueError("missing %s" % ".".join(keys))
        value = value[key]
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError("%s is not a count" % ".".join(keys))
    return value


def coverage(root):
    """(bound, total) of the invariant tables under strict contracts checking."""
    _errors, _pending, (total, bound, _waiting) = contracts_check.check(root, strict=True)
    return bound, total


def score(inp, bound, total):
    """Return (scorecard, failures) for input document `inp`; ValueError for a malformed input."""
    if not isinstance(inp, dict) or inp.get("format") != INPUT_FORMAT:
        raise ValueError("not %s" % INPUT_FORMAT)
    f5_enabled = inp.get("f5", {}).get("enabled")
    if not isinstance(f5_enabled, bool):
        raise ValueError("f5.enabled is not a boolean")
    commits = _count(inp, "commits")
    objects = _count(inp, "objects")
    changed = _count(inp, "changed_records")
    validators = _count(inp, "validators")
    invocations = _count(inp, "validator_invocations")
    card = {
        "format": OUTPUT_FORMAT,
        "f2": {"numerator": _count(inp, "f2", "numerator"), "denominator": _count(inp, "f2", "denominator")},
        "f5": {"numerator": _count(inp, "f5", "bypassing"), "denominator": _count(inp, "f5", "mutations"),
               "enabled": f5_enabled},
        "invariant_coverage": {"bound": bound, "total": total,
                               "ratio": round(bound / total, 6) if total else 0.0},
        "gate1": {"numerator": _count(inp, "gate1", "numerator"), "denominator": _count(inp, "gate1", "denominator")},
        "objects_per_commit": round(objects / commits, 3) if commits else 0.0,
        "validator_invocations": {"invocations": invocations, "bound": changed * validators,
                                  "changed_records": changed, "validators": validators},
        "types": {"run": _count(inp, "types"), "failed": _count(inp, "types_failed")},
    }
    failures = []
    if card["f2"]["numerator"] != 0:
        failures.append("F2: %d of %d domains have more than one owner (threshold 0)" %
                        (card["f2"]["numerator"], card["f2"]["denominator"]))
    if not f5_enabled:
        failures.append("F5: the ledger is compiled out (SL_FABRIC_F5_INSTRUMENT); nothing was measured")
    elif card["f5"]["numerator"] != 0:
        failures.append("F5: %d of %d store mutations bypassed the transaction manager (threshold 0)" %
                        (card["f5"]["numerator"], card["f5"]["denominator"]))
    elif card["f5"]["denominator"] == 0:
        failures.append("F5: the workload made no store mutation; nothing was measured")
    if total == 0 or bound < total:
        failures.append("coverage: %d of %d invariants bound (threshold 1.0)" % (bound, total))
    if card["gate1"]["numerator"] != 0:
        failures.append("gate 1: %d of %d records lack an owner, stable id or revision (threshold 0)" %
                        (card["gate1"]["numerator"], card["gate1"]["denominator"]))
    if card["types"]["failed"] != 0:
        failures.append("types: %d of %d types failed the conformance suite" %
                        (card["types"]["failed"], card["types"]["run"]))
    if invocations > changed * validators:
        failures.append("validators: %d invocations exceed changed records times validators (%d)" %
                        (invocations, changed * validators))
    card["failures"] = failures
    return card, failures


def main(argv):
    args = list(argv)
    root, out = ROOT, None
    for flag in ("--root", "--out"):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                print(USAGE, file=sys.stderr)
                return 2
            if flag == "--root":
                root = os.path.abspath(args[i + 1])
            else:
                out = os.path.abspath(args[i + 1])
            del args[i:i + 2]
    if len(args) != 1:
        print(USAGE, file=sys.stderr)
        return 2
    out = out or os.path.join(root, *OUT_PATH.split("/"))
    try:
        with open(args[0], encoding="utf-8") as fh:
            inp = json.load(fh)
        bound, total = coverage(root)
        card, failures = score(inp, bound, total)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        print("fabric-scorecard: %s: unreadable input: %s" % (args[0], exc), file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(card, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("f2 numerator %d denominator %d" % (card["f2"]["numerator"], card["f2"]["denominator"]))
    print("f5 numerator %d denominator %d" % (card["f5"]["numerator"], card["f5"]["denominator"]))
    print("invariant coverage %d/%d" % (bound, total))
    print("gate 1 %d/%d" % (card["gate1"]["numerator"], card["gate1"]["denominator"]))
    print("objects per commit %s" % card["objects_per_commit"])
    vi = card["validator_invocations"]
    print("validator invocations %d (bound %d = %d changed records x %d validators)" %
          (vi["invocations"], vi["bound"], vi["changed_records"], vi["validators"]))
    for line in failures:
        print("FAIL %s" % line)
    print("fabric-scorecard: %s (%s)" % ("failed" if failures else "ok", os.path.relpath(out, root)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
