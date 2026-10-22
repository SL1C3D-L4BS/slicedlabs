"""runtime-determinism's checker: the D1 hash matrix of the gate demo (WO-099, SL-ARCH-003 ARCH3-R170, ADR-0026).

Each run of `sl_runtime_demo --evidence-dir <runs>/<build>-w<workers>-<mode>` leaves hashes.json
(sl-runtime-hashes/1: seed, ticks, entities, workers, mode, compiler, resume_tick and the per-tick hash lists fresh,
resume and replay; fresh[k] is the state after tick k + 1, resume[k] after tick resume_tick + 1 + k, replay[k] after
tick k + 1). The runs of the primary build form the matrix: one row per (workers, mode, variant) with variant fresh,
resume (from the snapshot at resume_tick) or replay (of the recorded inputs), each compared tick by tick with the
reference, the fresh list of the primary build's run with the fewest workers in parallel mode. Every expected
(workers, mode) cell must be present. The runs of every other build are compared with the same reference and recorded
under `cross_compiler`; they never set the exit status (D1 is per compiler, ADR-0026).

Writes the matrix (sl-runtime-hash-matrix/1) to --out. Output: `determinism-check: ok (<n> rows)` or
`determinism-check: divergence at workers=<w> mode=<m> variant=<v> tick=<t>` (the first divergent row in matrix order:
workers ascending, parallel before serial, fresh, resume, replay) or one line per missing cell or input error. Exit 0
when every row equals the reference, 1 on a divergence or a missing cell, 2 on usage or unreadable or inconsistent
input. Stdlib only, no subprocess, no network (ADR-0004):
`python3 -I tools/runtime/determinism_check.py --runs DIR --primary BUILD [--workers 1,2,4,8,16]
[--modes parallel,serial] [--out FILE]`.
"""

import argparse
import json
import os
import re
import sys

FORMAT = "sl-runtime-hash-matrix/1"
HASHES_FORMAT = "sl-runtime-hashes/1"
DEFAULT_OUT = os.path.join("build", "runtime", "gate", "hash-matrix.json")
DEFAULT_WORKERS = (1, 2, 4, 8, 16)
MODES = ("parallel", "serial")
VARIANTS = ("fresh", "resume", "replay")
_RUN_DIR = re.compile(r"^(?P<build>.+)-w(?P<workers>[0-9]+)-(?P<mode>parallel|serial)$")
_HASH = re.compile(r"^sha256:[0-9a-f]{64}$")


class InputError(Exception):
    """An unreadable or inconsistent run (exit 2)."""


def _int(doc, key, where):
    value = doc.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise InputError("%s: %s is not a non-negative integer" % (where, key))
    return value


def _hashes(doc, key, where):
    value = doc.get(key)
    if not isinstance(value, list) or not all(isinstance(h, str) and _HASH.match(h) for h in value):
        raise InputError("%s: %s is not a list of sha256:<hex> hashes" % (where, key))
    return value


def load_run(path, build):
    """One run: its hashes.json fields plus the build label of its directory."""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise InputError("%s: %s" % (path, e)) from e
    if not isinstance(doc, dict) or doc.get("format") != HASHES_FORMAT:
        raise InputError("%s: format is not %s" % (path, HASHES_FORMAT))
    mode = doc.get("mode")
    if mode not in MODES:
        raise InputError("%s: mode is not parallel or serial" % path)
    run = {
        "path": path,
        "build": build,
        "compiler": str(doc.get("compiler", "unknown")),
        "mode": mode,
    }
    for key in ("seed", "ticks", "entities", "workers", "resume_tick"):
        run[key] = _int(doc, key, path)
    for key in VARIANTS:
        run[key] = _hashes(doc, key, path)
    if len(run["fresh"]) != run["ticks"]:
        raise InputError("%s: fresh holds %d hashes for %d ticks" % (path, len(run["fresh"]), run["ticks"]))
    return run


def load_runs(runs_dir):
    """Every <runs_dir>/<build>-w<N>-<mode>/hashes.json, sorted by directory name."""
    if not os.path.isdir(runs_dir):
        raise InputError("%s: not a directory" % runs_dir)
    runs = []
    for name in sorted(os.listdir(runs_dir)):
        m = _RUN_DIR.match(name)
        path = os.path.join(runs_dir, name, "hashes.json")
        if not m or not os.path.isfile(path):
            continue
        run = load_run(path, m.group("build"))
        if run["workers"] != int(m.group("workers")) or run["mode"] != m.group("mode"):
            raise InputError("%s: workers or mode differ from the directory name %s" % (path, name))
        runs.append(run)
    return runs


def first_divergence(reference, hashes, first_tick):
    """The first tick (1-based) where `hashes` (starting at first_tick) differs from the reference fresh list, or
    None. A list that runs past the reference or stops short diverges at the first tick it lacks or adds."""
    for k, h in enumerate(hashes):
        tick = first_tick + k
        if tick > len(reference) or reference[tick - 1] != h:
            return tick
    end = first_tick + len(hashes) - 1
    if end < len(reference):
        return end + 1
    return None


def _row(reference, run, variant):
    first_tick = run["resume_tick"] + 1 if variant == "resume" else 1
    hashes = run[variant]
    tick = first_divergence(reference, hashes, first_tick)
    return {
        "workers": run["workers"],
        "mode": run["mode"],
        "variant": variant,
        "first_tick": first_tick,
        "ticks": len(hashes),
        "equal": tick is None,
        "divergent_tick": tick,
        "final_state_hash": hashes[-1] if hashes else None,
    }


def _order(run):
    return (run["workers"], MODES.index(run["mode"]))


def build_matrix(runs, primary, workers=DEFAULT_WORKERS, modes=MODES):
    """(matrix document, problems) where problems are the exit-1 lines (missing cells, then the first divergence)."""
    main = sorted((r for r in runs if r["build"] == primary), key=_order)
    if not main:
        raise InputError("no run of build %s" % primary)
    scene = {k: main[0][k] for k in ("seed", "ticks", "entities")}
    for r in runs:
        for key, value in scene.items():
            if r[key] != value:
                raise InputError("%s: %s %d differs from %d" % (r["path"], key, r[key], value))
    cells = {}
    for r in main:
        if (r["workers"], r["mode"]) in cells:
            raise InputError("%s: a second run of workers %d mode %s" % (r["path"], r["workers"], r["mode"]))
        cells[(r["workers"], r["mode"])] = r
    parallel = [r for r in main if r["mode"] == "parallel"]
    ref_run = parallel[0] if parallel else main[0]
    reference = ref_run["fresh"]

    problems = []
    for w in workers:
        for m in modes:
            if (w, m) not in cells:
                problems.append("missing run workers=%d mode=%s" % (w, m))
    rows = [_row(reference, r, v) for r in main for v in VARIANTS]
    divergent = next((row for row in rows if not row["equal"]), None)
    if divergent is not None:
        problems.append("divergence at workers=%d mode=%s variant=%s tick=%d" % (
            divergent["workers"], divergent["mode"], divergent["variant"], divergent["divergent_tick"]))

    cross = []
    for r in sorted((r for r in runs if r["build"] != primary), key=lambda r: (r["build"],) + _order(r)):
        row = _row(reference, r, "fresh")
        row["build"] = r["build"]
        row["compiler"] = r["compiler"]
        cross.append(row)
    doc = {
        "format": FORMAT,
        "seed": scene["seed"],
        "ticks": scene["ticks"],
        "entities": scene["entities"],
        "reference": {"build": primary, "compiler": ref_run["compiler"], "workers": ref_run["workers"],
                      "mode": ref_run["mode"], "variant": "fresh",
                      "final_state_hash": reference[-1] if reference else None},
        "expected": {"workers": list(workers), "modes": list(modes), "variants": list(VARIANTS)},
        "rows": rows,
        "all_equal": divergent is None,
        "missing": [p for p in problems if p.startswith("missing")],
        "first_divergence": None if divergent is None else {
            k: divergent[k] for k in ("workers", "mode", "variant")} | {"tick": divergent["divergent_tick"]},
        "cross_compiler": {
            "recorded_only": True,
            "runs": cross,
            "all_equal": all(row["equal"] for row in cross) if cross else None,
        },
    }
    return doc, problems


def _int_list(text):
    try:
        values = tuple(int(v) for v in text.split(","))
    except ValueError as e:
        raise argparse.ArgumentTypeError("not a comma-separated list of integers: %s" % text) from e
    if not values or any(v < 1 for v in values):
        raise argparse.ArgumentTypeError("workers must be 1 or more: %s" % text)
    return values


def _mode_list(text):
    values = tuple(text.split(","))
    if not values or any(v not in MODES for v in values):
        raise argparse.ArgumentTypeError("modes are parallel and serial: %s" % text)
    return values


def main(argv=None, out=sys.stdout):
    parser = argparse.ArgumentParser(prog="determinism_check.py", description=__doc__.splitlines()[0])
    parser.add_argument("--runs", required=True, help="the directory holding <build>-w<N>-<mode>/hashes.json")
    parser.add_argument("--primary", required=True, help="the build whose runs form the matrix")
    parser.add_argument("--workers", type=_int_list, default=DEFAULT_WORKERS)
    parser.add_argument("--modes", type=_mode_list, default=MODES)
    parser.add_argument("--out", default=DEFAULT_OUT)
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    try:
        doc, problems = build_matrix(load_runs(args.runs), args.primary, args.workers, args.modes)
    except InputError as e:
        print("determinism-check: %s" % e, file=out)
        return 2
    parent = os.path.dirname(args.out)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    os.replace(tmp, args.out)
    for p in problems:
        print("determinism-check: %s" % p, file=out)
    cross = doc["cross_compiler"]
    if cross["runs"]:
        print("determinism-check: cross_compiler (recorded only): %s" % (
            "equal" if cross["all_equal"] else "differs"), file=out)
    if problems:
        return 1
    print("determinism-check: ok (%d rows, final %s)" % (len(doc["rows"]), doc["reference"]["final_state_hash"]),
          file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
