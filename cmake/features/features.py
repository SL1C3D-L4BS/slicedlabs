#!/usr/bin/python3 -I
# sliced:summary=Measure, check and report the C++ feature battery (cmake/SlFeatures.cmake) on clang and gcc.
# sliced:args=measure [--apply] | check | report
"""features.py: the driver behind `just features-measure`, `features-check` and `cxx26-report` (WO-015).
The justfile runs cmake/features/run.sh first (it configures the debug (clang) and gcc-debug presets in
build/features-measure/<preset>, not build/<preset>, so a parallel build is not disturbed); this script only reads
<build>/features/results.json and the exit codes, and spawns nothing (ADR-0004).
  measure [--apply]   fresh cache; write cmake/features/matrix.json and the reasons in deferred-cxx23.txt.
                      --apply also rewrites both lists: ids ok on clang and gcc become required, the rest deferred
                      (the first population; later changes to the required list go through an ADR).
  check               fresh cache; exit 1 when a C++23 result differs from matrix.json or the required list is
                      not the one measured; C++26 changes are printed and never fail.
  report              cached configure; print the C++26 report; exit 0 unless the configure itself stops.
MSVC and clang-cl cannot be measured here: their matrix columns stay "unverified"."""
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
FEATURES = ROOT / "cmake" / "features"
MATRIX = FEATURES / "matrix.json"
REQUIRED = FEATURES / "required-cxx23.txt"
DEFERRED = FEATURES / "deferred-cxx23.txt"
PRESETS = ("debug", "gcc-debug")
KEYS = ("clang", "gcc", "msvc", "clang-cl")
MEASURED = ("clang", "gcc")


def read_ids(path):
    ids = []
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                ids.append(line)
    return ids


def norm(status):
    return "ok" if status == "ok" else "fail"


def load(preset):
    """The outcome of cmake/features/run.sh for one preset: (exit code, log text, results.json or None)."""
    bdir = ROOT / "build" / "features-measure"
    rc_file, log_file = bdir / f"{preset}.rc", bdir / f"{preset}.log"
    rc = int(rc_file.read_text().strip()) if rc_file.exists() else 1
    log = log_file.read_text() if log_file.exists() else ""
    results = bdir / preset / "features" / "results.json"
    return rc, log, (json.loads(results.read_text()) if results.exists() else None)


def run_all(fresh):
    """Collect {compiler key: results} that run.sh just produced. ok is False when a configure failed."""
    by_key = {}
    ok = True
    for preset in PRESETS:
        rc, log, data = load(preset)
        if data is None or rc != 0:
            ok = False
            print(f"features: configure --preset {preset} failed (exit {rc}):", file=sys.stderr)
            print("\n".join(l for l in log.splitlines() if l.strip())[-1800:], file=sys.stderr)
        if data is not None:
            by_key[data["compiler"]["key"]] = data
    return ok, by_key


def build_matrix(by_key, required):
    ids23 = sorted(set().union(*[set(d["cxx23"]) for d in by_key.values()]))
    ids26 = sorted(set().union(*[set(d["cxx26"]) for d in by_key.values()]))
    m = {"schema": "sl-features-matrix/1", "compilers": {}, "cxx23": {}, "cxx26": {}}
    for key in KEYS:
        d = by_key.get(key)
        m["compilers"][key] = ({"compiler": f"{d['compiler']['id']} {d['compiler']['version']}",
                                "stdlib": d["compiler"]["stdlib"], "preset": d["compiler"]["preset"],
                                "flags_cxx23": d["compiler"]["flags_cxx23"]} if d else "unverified")
    for i in ids23:
        row = {k: (norm(by_key[k]["cxx23"][i]["status"]) if k in by_key and i in by_key[k]["cxx23"] else "unverified")
               for k in KEYS}
        row["required"] = i in required
        m["cxx23"][i] = row
    for i in ids26:
        row = {k: (norm(by_key[k]["cxx26"][i]["status"]) if k in by_key and i in by_key[k]["cxx26"] else "unverified")
               for k in KEYS}
        m["cxx26"][i] = row
    return m


def reason(by_key, i):
    parts = []
    for k in MEASURED:
        d = by_key.get(k, {}).get("cxx23", {}).get(i)
        if d and d["status"] != "ok":
            msg = d["detail"].split("error:", 1)[-1].replace("#error", "").strip().strip('"')
            parts.append(f"{k}: {msg[:90].rstrip(' \"')}")
    return "; ".join(parts) or "passes on clang and gcc, promote through an ADR"


def cmd_measure(apply):
    ok, by_key = run_all(True)
    if not ok or not all(k in by_key for k in MEASURED):
        return 1
    ids = sorted(set(by_key["clang"]["cxx23"]) | set(by_key["gcc"]["cxx23"]))
    passing = {i for i in ids if all(by_key[k]["cxx23"].get(i, {}).get("status") == "ok" for k in MEASURED)}
    required = read_ids(REQUIRED)
    deferred = read_ids(DEFERRED)
    if apply:
        required = sorted(passing)
        deferred = sorted(set(ids) - passing)
        REQUIRED.write_text("# Required C++23 features (one id per line); each has cmake/features/cxx23/<id>.cpp.\n"
                            "# Decided by `just features-measure --apply` on clang and gcc; changes go through an ADR\n"
                            "# (SL-GOV-004) and a change to SL-EP-043.\n" + "".join(f"{i}\n" for i in required))
    DEFERRED.write_text("# Deferred C++23 candidates: fail on a supported compiler; tested and reported, never fatal.\n"
                        "# The comment is the measured reason (rewritten by `just features-measure`).\n"
                        + "".join(f"{i}  # {reason(by_key, i)}\n" for i in sorted(deferred)))
    MATRIX.write_text(json.dumps(build_matrix(by_key, set(required)), indent=2, sort_keys=True) + "\n")
    bad = [i for i in required if i not in passing]
    for i in bad:
        print(f"features-measure: required '{i}' fails ({reason(by_key, i)}); remove it through an ADR (SL-GOV-004)")
    for i in sorted(set(deferred) & passing):
        print(f"features-measure: deferred '{i}' now passes on clang and gcc; promote it through an ADR")
    print(f"features-measure: {len(required)} required, {len(deferred)} deferred; wrote {MATRIX.relative_to(ROOT)}")
    return 1 if bad else 0


def cmd_check():
    ok, by_key = run_all(True)
    if not ok or not all(k in by_key for k in MEASURED):
        return 1
    if not MATRIX.exists():
        print("features-check: cmake/features/matrix.json is missing; run `just features-measure`")
        return 1
    old = json.loads(MATRIX.read_text())
    new = build_matrix(by_key, set(read_ids(REQUIRED)))
    drift = []
    for i in sorted(set(old["cxx23"]) | set(new["cxx23"])):
        o, n = old["cxx23"].get(i), new["cxx23"].get(i)
        if o is None or n is None:
            drift.append(f"{i}: {'new test, not in matrix.json' if o is None else 'in matrix.json, no longer tested'}")
            continue
        for k in MEASURED + ("required",):
            if o.get(k) != n.get(k):
                drift.append(f"{i}: {k} was {o.get(k)}, now {n.get(k)}")
    changes26 = []
    for i in sorted(set(old["cxx26"]) | set(new["cxx26"])):
        o, n = old["cxx26"].get(i, {}), new["cxx26"].get(i, {})
        for k in MEASURED:
            if o.get(k) != n.get(k):
                changes26.append(f"{i}: {k} was {o.get(k, 'absent')}, now {n.get(k, 'absent')}")
    for c in changes26:
        print(f"features-check: C++26 change (not a failure): {c}")
    for k in MEASURED:
        o, n = old["compilers"].get(k), new["compilers"].get(k)
        if o != n and isinstance(o, dict) and isinstance(n, dict) and o["compiler"] != n["compiler"]:
            print(f"features-check: {k} is now {n['compiler']} (matrix.json was measured with {o['compiler']})")
    if drift:
        for d in drift:
            print(f"features-check: C++23 drift: {d}")
        print("features-check: FAIL: re-read the cxx-standards notes, then `just features-measure`")
        return 1
    n_req = sum(1 for v in new["cxx23"].values() if v["required"])
    print(f"features-check: ok ({n_req} required, {len(new['cxx23']) - n_req} deferred match matrix.json on "
          f"{', '.join(MEASURED)}; {len(changes26)} C++26 changes)")
    return 0


def cmd_report():
    ok, by_key = run_all(False)
    for preset in PRESETS:
        path = ROOT / "build" / "features-measure" / preset / "features" / "cxx26-report.json"
        if not path.exists():
            continue
        rep = json.loads(path.read_text())
        c = rep["compiler"]
        print(f"C++26 probes, {c['id']} {c['version']} ({c['stdlib']}), report only:")
        for i, p in rep["probes"].items():
            note = f"  ({p['detail'][:110]})" if p["status"] != "ok" and p["detail"] else ""
            print(f"  {i:24} {p['status']}{note}")
    return 0 if ok else 1


def main(argv):
    if argv[:1] == ["measure"] and set(argv[1:]) <= {"--apply"}:
        return cmd_measure("--apply" in argv)
    if argv == ["check"]:
        return cmd_check()
    if argv == ["report"]:
        return cmd_report()
    print("usage: features.py measure [--apply] | check | report", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
