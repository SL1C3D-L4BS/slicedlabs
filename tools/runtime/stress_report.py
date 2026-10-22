"""stress-report: the stress matrix result from ctest JUnit (WO-098, SL-ARCH-003 ARCH3-R162 to R166, episode 099).

Reads the JUnit file that `ctest --output-junit` wrote for the runtime_stress label (scripts/runtime/runtime-stress.sh
writes build/runtime/stress/junit.xml) and writes sl-runtime-stress-matrix/1 (schemas/runtime/stress-matrix.schema.json)
to build/runtime/gate/stress-matrix.json: one cell per stress_matrix row S1-S10 for the preset, each with pass (the
ctest entry of its file passed), wall_s (that entry's time; two cells of one file share it) and seed. The seed is the
one of the "seed <s>; rerun: ..." line printed under the cell's test case when it failed, else SL_RUNTIME_SOAK_SEED
when set (runtime-soak), else the cell's fixed seed. stress_failpoints_test has no S cell and is not a matrix entry.

With --merge the cells of other presets already in the output file are kept (one file across debug and the
sanitizer presets); cells of the same preset are replaced. The written document is validated against the schema.
A stress file missing from the JUnit file, a malformed JUnit file or a schema finding is exit 1; usage is exit 2;
otherwise exit 0, failed cells included (the caller's ctest exit decides the gate). Stdlib only, no subprocess, no
network (ADR-0004): `python3 -I tools/runtime/stress_report.py [--junit F] [--out F] [--preset P] [--scale S]
[--merge]`.
"""

import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import slib  # noqa: E402

FORMAT = "sl-runtime-stress-matrix/1"
SCHEMA_PATH = os.path.join("schemas", "runtime", "stress-matrix.schema.json")
DEFAULT_JUNIT = os.path.join("build", "runtime", "stress", "junit.xml")
DEFAULT_OUT = os.path.join("build", "runtime", "gate", "stress-matrix.json")
PRESETS = ("debug", "ci-asan", "ci-ubsan", "ci-tsan")
SANITIZER_SCALE = 0.1

# cell -> (test file stem, fixed seed of the cell's test case)
CELLS = {
    "S1": ("stress_jobs", 0x5EED0990001),
    "S2": ("stress_deque", 0x5EED0990002),
    "S3": ("stress_jobs", 0x5EED0990003),
    "S4": ("stress_alloc", 0x5EED0990004),
    "S5": ("stress_alloc", 0x5EED0990005),
    "S6": ("stress_ecs", 0x5EED0990006),
    "S7": ("stress_det_matrix", 0x5EED0990017),
    "S8": ("stress_lifecycle", 0x5EED0990008),
    "S9": ("stress_lifecycle", 0x5EED0990009),
    "S10": ("stress_events", 0x5EED099000A),
}
_CASE = re.compile(r"TEST CASE:\s+(.*)")
_CELL = re.compile(r"\bS(10|[1-9])\b")
_SEED = re.compile(r"\bseed (\d+); rerun\b")


class ReportError(Exception):
    pass


def ctest_name(stem):
    return "runtime_" + stem + "_test"


def test_path(stem):
    return "tests/integration/runtime/" + stem + "_test.cpp"


def parse_junit(text):
    """{ctest name: (passed, time_s, output)} of the JUnit testcases."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        raise ReportError("malformed JUnit: %s" % exc) from exc
    out = {}
    for case in root.iter("testcase"):
        name = case.get("name")
        if not name:
            raise ReportError("a testcase without a name")
        try:
            wall = float(case.get("time", "0"))
        except ValueError as exc:
            raise ReportError("testcase %s: time %r is not a number" % (name, case.get("time"))) from exc
        status = case.get("status", "run")
        failed = any(case.find(tag) is not None for tag in ("failure", "error", "skipped")) or status not in (
            "run",
            "passed",
        )
        texts = [node.text or "" for node in case.iter() if node.tag in ("system-out", "system-err", "failure")]
        texts += [node.get("message", "") for node in case.iter("failure")]
        out[name] = (not failed, max(wall, 0.0), "\n".join(texts))
    return out


def failed_seeds(output):
    """{cell: seed} from the rerun lines printed under each failing test case of `output`."""
    seeds = {}
    cell = None
    for line in output.splitlines():
        case = _CASE.search(line)
        if case:
            found = _CELL.search(case.group(1))
            cell = "S" + found.group(1) if found else None
            continue
        seed = _SEED.search(line)
        if seed and cell and cell not in seeds:
            seeds[cell] = int(seed.group(1))
    return seeds


def soak_seed(environ):
    value = environ.get("SL_RUNTIME_SOAK_SEED", "").strip()
    if not value:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def build_cells(results, preset, environ):
    cells = []
    missing = sorted({stem for stem, _ in CELLS.values() if ctest_name(stem) not in results})
    if missing:
        raise ReportError("no JUnit testcase for " + ", ".join(ctest_name(s) for s in missing))
    soak = soak_seed(environ)
    for cell, (stem, fixed) in sorted(CELLS.items(), key=lambda kv: int(kv[0][1:])):
        passed, wall, output = results[ctest_name(stem)]
        seed = failed_seeds(output).get(cell)
        if seed is None:
            seed = soak if soak is not None else fixed
        cells.append(
            {
                "cell": cell,
                "preset": preset,
                "test": test_path(stem),
                "pass": passed,
                "wall_s": round(wall, 3),
                "seed": seed,
            }
        )
    return cells


def default_scale(preset, environ):
    value = environ.get("SL_RUNTIME_STRESS_SCALE", "").strip()
    if value:
        try:
            return max(float(value), 0.0)
        except ValueError:
            pass
    return 1.0 if preset == "debug" else SANITIZER_SCALE


def merge(previous, cells, preset):
    """The cells of `previous` (a matrix document or None) for other presets, then `cells`."""
    kept = []
    if isinstance(previous, dict) and isinstance(previous.get("cells"), list):
        kept = [c for c in previous["cells"] if isinstance(c, dict) and c.get("preset") != preset]
    return kept + cells


def make_document(junit_text, preset, scale, environ, previous=None):
    cells = build_cells(parse_junit(junit_text), preset, environ)
    return {"format": FORMAT, "scale": scale, "cells": merge(previous, cells, preset)}


def _usage(err):
    err.write(
        "usage: stress_report.py [--junit FILE] [--out FILE] [--preset debug|ci-asan|ci-ubsan|ci-tsan] "
        "[--scale S] [--merge]\n"
    )
    return 2


def parse_args(argv):
    opts = {"junit": DEFAULT_JUNIT, "out": DEFAULT_OUT, "preset": "debug", "scale": None, "merge": False}
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--merge":
            opts["merge"] = True
            i += 1
            continue
        if arg in ("--junit", "--out", "--preset", "--scale") and i + 1 < len(argv):
            opts[arg[2:]] = argv[i + 1]
            i += 2
            continue
        return None
    if opts["preset"] not in PRESETS:
        return None
    if opts["scale"] is not None:
        try:
            opts["scale"] = float(opts["scale"])
        except ValueError:
            return None
        if opts["scale"] < 0:
            return None
    return opts


def main(argv, root=ROOT, environ=None, out=None, err=None):
    out = out or sys.stdout
    err = err or sys.stderr
    environ = os.environ if environ is None else environ
    opts = parse_args(argv)
    if opts is None:
        return _usage(err)
    junit = os.path.join(root, opts["junit"])
    target = os.path.join(root, opts["out"])
    scale = opts["scale"] if opts["scale"] is not None else default_scale(opts["preset"], environ)
    try:
        with open(junit, encoding="utf-8") as f:
            text = f.read()
    except OSError as exc:
        err.write("stress-report: %s: %s\n" % (opts["junit"], exc.strerror))
        return 1
    previous = None
    if opts["merge"] and os.path.exists(target):
        try:
            previous = slib.load_json(target)
        except slib.LoadError as exc:
            err.write("stress-report: %s: %s\n" % (opts["out"], exc))
            return 1
    try:
        doc = make_document(text, opts["preset"], scale, environ, previous)
    except ReportError as exc:
        err.write("stress-report: %s: %s\n" % (opts["junit"], exc))
        return 1
    findings = slib.validate(doc, slib.load_json(os.path.join(root, SCHEMA_PATH)))
    if findings:
        for path, message in findings:
            err.write("stress-report: %s: %s\n" % (path or "/", message))
        return 1
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=False)
        f.write("\n")
    for cell in doc["cells"]:
        if cell["preset"] == opts["preset"]:
            out.write(
                "stress-report: %s %s %s %.3f s seed %d\n"
                % (cell["cell"], cell["preset"], "pass" if cell["pass"] else "FAIL", cell["wall_s"], cell["seed"])
            )
    out.write("stress-report: wrote %s\n" % opts["out"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
