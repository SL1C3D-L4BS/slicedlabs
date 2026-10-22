"""runtime-scorecard: the Volume 05 scorecard from the gate evidence (WO-100; SL-ARCH-003 ARCH3-R171, RT-INV-GATE-07).

Inputs, under --gate (default build/runtime/gate) and --san (default build/runtime/san):

  demo-report.json    sl-runtime-gate/1 (just runtime-demo): recovery.hash_equal          required
  memreport.json      sl-runtime-memreport/1 (just runtime-demo): leaks[]                  required
  f5.json             sl-runtime-f5/1 (runtime_gate_fabric_test): bypassing, mutations     required
  stress-matrix.json  sl-runtime-stress-matrix/1 (just runtime-stress): cells[].pass         required
  hash-matrix.json    sl-runtime-hash-matrix/1 (just runtime-determinism, check-full)      optional
  <san>-*.log         just runtime-san asan|ubsan|tsan (check-full): report lines           optional

and runtime-contracts-strict's summary, computed here with tools/runtime/contracts_check.py in strict mode.

Output: build/runtime/scorecard.json (format sl-runtime-scorecard/1, or --out PATH), one record per measure with a
`status` of pass, fail or not_run:

  leaks               {count, status}: leak entries of the shutdown memory report (threshold 0)
  sanitizer_reports   {count, logs, by_sanitizer, status}: sanitizer report lines in the logs (threshold 0)
  divergences         {count, cells, recovery_hash_equal, status}: divergent hash-matrix cells and a recovery whose
                      hash differs (threshold 0); without hash-matrix.json only the recovery is measured
  f5_bypassing        {numerator, denominator, enabled, status}: Fabric mutations bypassing the transaction manager
                      over all store mutations (threshold 0; a compiled-out ledger measured nothing and fails)
  stress_failures     {count, cells, missing, status}: failed or missing cells of S1 to S10 (threshold 0)
  invariant_coverage  {bound, total, ratio, errors, status}: threshold 1.0 and no strict contracts error
  failures            one line per failed measure, starting with the measure's name

An optional input that is absent is `not_run` (never pass) and does not fail the run; a required input that is
absent or unreadable fails its measure with `not_run` in the line. Exit 1 when any measure fails, 2 on bad usage.
Stdlib only, no subprocess (ADR-0004).

  python3 -I tools/runtime/scorecard.py [--gate DIR] [--san DIR] [--out PATH] [--root DIR]
"""

import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)


def _sibling(name):
    """tools/runtime/<name>.py under a unique module name: tools/fabric has modules of the same names (tools-test)."""
    key = "runtime_" + name
    if key not in sys.modules:
        spec = importlib.util.spec_from_file_location(key, os.path.join(HERE, name + ".py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[key] = module
        spec.loader.exec_module(module)
    return sys.modules[key]


contracts_check = _sibling("contracts_check")

OUTPUT_FORMAT = "sl-runtime-scorecard/1"
OUT_PATH = "build/runtime/scorecard.json"
GATE_DIR = "build/runtime/gate"
SAN_DIR = "build/runtime/san"
SANITIZERS = ("asan", "ubsan", "tsan")
CELLS = tuple("S%d" % n for n in range(1, 11))
# the same report pattern as scripts/runtime/runtime-san.sh
REPORT = re.compile(r"ThreadSanitizer:|AddressSanitizer:|LeakSanitizer:|runtime error:")
FORMATS = {"demo-report.json": "sl-runtime-gate/1", "memreport.json": "sl-runtime-memreport/1",
           "f5.json": "sl-runtime-f5/1", "stress-matrix.json": "sl-runtime-stress-matrix/1",
           "hash-matrix.json": "sl-runtime-hash-matrix/1"}
MEASURES = ("leaks", "sanitizer_reports", "divergences", "f5_bypassing", "stress_failures", "invariant_coverage")
USAGE = "usage: scorecard.py [--gate DIR] [--san DIR] [--out PATH] [--root DIR]"


def _count(value, what):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError("%s is not a count" % what)
    return value


def load(gate, name):
    """(document, None) or (None, reason) for one evidence file; the format must match."""
    path = os.path.join(gate, name)
    if not os.path.isfile(path):
        return None, "%s is missing" % name
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        return None, "%s is unreadable: %s" % (name, exc)
    if not isinstance(doc, dict) or doc.get("format") != FORMATS[name]:
        return None, "%s is not %s" % (name, FORMATS[name])
    return doc, None


def leaks(gate):
    doc, why = load(gate, "memreport.json")
    if doc is None:
        return {"count": None, "status": "not_run"}, "leaks: not_run: %s (just runtime-demo)" % why
    entries = doc.get("leaks")
    if not isinstance(entries, list):
        return {"count": None, "status": "fail"}, "leaks: memreport.json has no leaks list"
    n = len(entries)
    if n:
        return {"count": n, "status": "fail"}, "leaks: %d live blocks at shutdown (threshold 0)" % n
    return {"count": 0, "status": "pass"}, None


def sanitizer_reports(san):
    names = sorted(os.listdir(san)) if os.path.isdir(san) else []
    logs = [n for n in names if n.endswith(".log") and n.split("-", 1)[0] in SANITIZERS]
    if not logs:
        return {"count": None, "logs": 0, "by_sanitizer": {}, "status": "not_run"}, None
    by, hits = {s: None for s in SANITIZERS}, []
    for name in logs:
        kind = name.split("-", 1)[0]
        try:
            with open(os.path.join(san, name), encoding="utf-8", errors="replace") as fh:
                lines = [ln.rstrip("\n") for ln in fh if REPORT.search(ln)]
        except OSError as exc:
            return ({"count": None, "logs": len(logs), "by_sanitizer": {}, "status": "fail"},
                    "sanitizer_reports: %s is unreadable: %s" % (name, exc))
        by[kind] = (by[kind] or 0) + len(lines)
        hits.extend("%s: %s" % (name, ln.strip()) for ln in lines)
    rec = {"count": len(hits), "logs": len(logs), "by_sanitizer": {k: v for k, v in by.items() if v is not None},
           "status": "fail" if hits else "pass"}
    if hits:
        return rec, "sanitizer_reports: %d report lines (threshold 0); first: %s" % (len(hits), hits[0])
    return rec, None


def divergences(gate):
    demo, why = load(gate, "demo-report.json")
    if demo is None:
        return ({"count": None, "cells": None, "recovery_hash_equal": None, "status": "not_run"},
                "divergences: not_run: %s (just runtime-demo)" % why)
    equal = demo.get("recovery", {}).get("hash_equal") if isinstance(demo.get("recovery"), dict) else None
    if not isinstance(equal, bool):
        return ({"count": None, "cells": None, "recovery_hash_equal": None, "status": "fail"},
                "divergences: demo-report.json has no recovery.hash_equal")
    count = 0 if equal else 1
    matrix, _why = load(gate, "hash-matrix.json")
    cells = None
    if matrix is not None:
        rows = matrix.get("rows")
        missing = matrix.get("missing")
        if not isinstance(rows, list) or not isinstance(missing, list):
            return ({"count": None, "cells": None, "recovery_hash_equal": equal, "status": "fail"},
                    "divergences: hash-matrix.json has no rows or missing list")
        cells = sum(1 for r in rows if not (isinstance(r, dict) and r.get("equal") is True)) + len(missing)
        count += cells
    rec = {"count": count, "cells": cells, "recovery_hash_equal": equal, "status": "fail" if count else "pass"}
    if count:
        parts = []
        if not equal:
            parts.append("the recovery hash differs from the fresh run")
        if cells:
            parts.append("%d hash-matrix cells divergent or missing" % cells)
        return rec, "divergences: %d (threshold 0): %s" % (count, "; ".join(parts))
    return rec, None


def f5_bypassing(gate):
    doc, why = load(gate, "f5.json")
    if doc is None:
        return ({"numerator": None, "denominator": None, "enabled": None, "status": "not_run"},
                "f5_bypassing: not_run: %s (runtime_gate_fabric_test)" % why)
    enabled = doc.get("enabled")
    try:
        num, den = _count(doc.get("bypassing"), "bypassing"), _count(doc.get("mutations"), "mutations")
    except ValueError as exc:
        return {"numerator": None, "denominator": None, "enabled": enabled, "status": "fail"}, "f5_bypassing: %s" % exc
    rec = {"numerator": num, "denominator": den, "enabled": enabled, "status": "fail"}
    if enabled is not True:
        return rec, "f5_bypassing: the F5 ledger is compiled out (SL_FABRIC_F5_INSTRUMENT); nothing was measured"
    if num:
        return rec, "f5_bypassing: %d of %d store mutations bypassed the transaction manager (threshold 0)" % (num, den)
    if den == 0:
        return rec, "f5_bypassing: the workload made no store mutation; nothing was measured"
    rec["status"] = "pass"
    return rec, None


def stress_failures(gate):
    doc, why = load(gate, "stress-matrix.json")
    if doc is None:
        return ({"count": None, "cells": [], "missing": [], "status": "not_run"},
                "stress_failures: not_run: %s (just runtime-stress)" % why)
    rows = doc.get("cells")
    if not isinstance(rows, list):
        return {"count": None, "cells": [], "missing": [], "status": "fail"}, "stress_failures: no cells list"
    seen = {r.get("cell"): r for r in rows if isinstance(r, dict)}
    failed = [c for c in CELLS if c in seen and seen[c].get("pass") is not True]
    missing = [c for c in CELLS if c not in seen]
    count = len(failed) + len(missing)
    rec = {"count": count, "cells": failed, "missing": missing, "status": "fail" if count else "pass"}
    if count:
        return rec, "stress_failures: %d (threshold 0): failed %s, missing %s" % (
            count, ", ".join(failed) or "none", ", ".join(missing) or "none")
    return rec, None


def invariant_coverage(root):
    errors, _pending, (total, bound, _waiting) = contracts_check.check(root, strict=True)
    ratio = round(bound / total, 6) if total else 0.0
    rec = {"bound": bound, "total": total, "ratio": ratio, "errors": len(errors),
           "status": "pass" if total and bound == total and not errors else "fail"}
    if rec["status"] == "fail":
        return rec, "invariant_coverage: %d of %d invariants bound, %d strict errors (threshold 1.0)" % (
            bound, total, len(errors))
    return rec, None


def score(gate, san, root, coverage=None):
    """Return (scorecard, failures). `coverage` replaces the strict contracts run (tests)."""
    card, failures = {"format": OUTPUT_FORMAT}, []
    results = [("leaks", leaks(gate)), ("sanitizer_reports", sanitizer_reports(san)),
               ("divergences", divergences(gate)), ("f5_bypassing", f5_bypassing(gate)),
               ("stress_failures", stress_failures(gate)),
               ("invariant_coverage", coverage if coverage is not None else invariant_coverage(root))]
    for name, (rec, line) in results:
        card[name] = rec
        if line is not None:
            failures.append(line)
    card["failures"] = failures
    return card, failures


def main(argv):
    args = list(argv)
    opts = {"--root": ROOT, "--gate": None, "--san": None, "--out": None}
    for flag in list(opts):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                print(USAGE, file=sys.stderr)
                return 2
            opts[flag] = os.path.abspath(args[i + 1])
            del args[i:i + 2]
    if args:
        print(USAGE, file=sys.stderr)
        return 2
    root = opts["--root"]
    gate = opts["--gate"] or os.path.join(root, *GATE_DIR.split("/"))
    san = opts["--san"] or os.path.join(root, *SAN_DIR.split("/"))
    out = opts["--out"] or os.path.join(root, *OUT_PATH.split("/"))
    card, failures = score(gate, san, root)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(card, fh, indent=2, sort_keys=True)
        fh.write("\n")
    for name in MEASURES:
        rec = card[name]
        detail = " ".join("%s=%s" % (k, json.dumps(rec[k], sort_keys=True)) for k in sorted(rec) if k != "status")
        print("%-18s %-7s %s" % (name, rec["status"], detail))
    for line in failures:
        print("FAIL %s" % line)
    print("runtime-scorecard: %s (%s)" % ("failed" if failures else "ok", os.path.relpath(out, root)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
