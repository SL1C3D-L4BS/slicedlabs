"""presets-check: structural check of CMakePresets.json (ADR-0005, WO-014). Stdlib only; run with `python3 -I`.

Usage: python3 -I tools/presets/check.py [path/to/CMakePresets.json]

Rules (each failure prints `presets-check: FAIL: <rule>: <detail>`):
  schema     version 8, cmakeMinimumRequired 3.28
  required   the hidden bases and the visible preset names of the plan exist
  pair       every configure preset has a build preset and a test preset of the same name that point back at it;
             every test preset has noTestsAction error and outputOnFailure
  inherits   every `inherits` name exists as a configure preset; no cycle; the plan's bases are hidden
  workflow   every workflow step names an existing preset of its type; configure, build and test appear
  windows    win-* presets carry a hostSystemName == Windows condition (also on their build and test presets)
  vcpkg      vcpkg-debug carries a notEquals $env{VCPKG_ROOT} "" condition and the vcpkg toolchain file
  binarydir  the effective binaryDir is ${sourceDir}/build/${presetName} for every configure preset
  dupkeys    no cache key is defined twice along an inherits chain
"""

import json
import os
import sys

HIDDEN_BASES = ["base", "clang", "gcc", "msvc", "clangcl", "ci"]
VISIBLE = [
    "debug", "release", "asan", "ubsan", "tsan", "gcc-debug", "gcc-release", "ci-clang", "ci-gcc",
    "ci-asan", "ci-ubsan", "ci-tsan", "ci-release", "cxx26", "win-msvc-debug", "win-clangcl-debug",
    "vcpkg-debug",
]
WORKFLOWS = ["debug", "gcc-debug", "ci-clang", "ci-gcc", "ci-asan"]
BINARY_DIR = "${sourceDir}/build/${presetName}"
WINDOWS_COND = {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows"}
VCPKG_COND = {"type": "notEquals", "lhs": "$env{VCPKG_ROOT}", "rhs": ""}
VCPKG_TOOLCHAIN = "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"


def as_list(value):
    if value is None:
        return []
    return [value] if isinstance(value, str) else list(value)


def check(doc):
    errors = []

    def fail(rule, detail):
        errors.append("%s: %s" % (rule, detail))

    if doc.get("version") != 8:
        fail("schema", "version must be 8, got %r" % (doc.get("version"),))
    cmr = doc.get("cmakeMinimumRequired") or {}
    if (cmr.get("major"), cmr.get("minor")) != (3, 28):
        fail("schema", "cmakeMinimumRequired must be 3.28, got %r" % (cmr,))

    configure = {}
    for p in doc.get("configurePresets", []):
        if p["name"] in configure:
            fail("required", "duplicate configure preset %s" % p["name"])
        configure[p["name"]] = p
    build = {p["name"]: p for p in doc.get("buildPresets", [])}
    test = {p["name"]: p for p in doc.get("testPresets", [])}
    workflow = {p["name"]: p for p in doc.get("workflowPresets", [])}

    for name in HIDDEN_BASES:
        if name not in configure:
            fail("required", "hidden base %s is missing" % name)
        elif not configure[name].get("hidden"):
            fail("inherits", "base %s must be hidden" % name)
    for name in VISIBLE:
        if name not in configure:
            fail("required", "configure preset %s is missing" % name)
        elif configure[name].get("hidden"):
            fail("required", "configure preset %s must be visible" % name)
    for name in WORKFLOWS:
        if name not in workflow:
            fail("required", "workflow preset %s is missing" % name)

    # inherits
    for name, p in configure.items():
        for parent in as_list(p.get("inherits")):
            if parent not in configure:
                fail("inherits", "%s inherits %s, which does not exist" % (name, parent))

    def chain(name, seen=()):
        """All ancestors of a configure preset, nearest first; stops (and reports) on a cycle."""
        out = []
        for parent in as_list(configure[name].get("inherits")):
            if parent not in configure:
                continue
            if parent in seen or parent == name:
                fail("inherits", "cycle through %s" % parent)
                continue
            out.append(parent)
            out.extend(chain(parent, seen + (name,)))
        return out

    for name in configure:
        ancestors = chain(name)
        # binaryDir: the nearest definition wins
        eff = None
        for n in [name] + ancestors:
            if "binaryDir" in configure[n]:
                eff = configure[n]["binaryDir"]
                break
        if not configure[name].get("hidden") and eff != BINARY_DIR:
            fail("binarydir", "%s binaryDir is %r, want %s" % (name, eff, BINARY_DIR))
        if "binaryDir" in configure[name] and configure[name]["binaryDir"] != BINARY_DIR:
            fail("binarydir", "%s defines binaryDir %r, want %s" % (name, configure[name]["binaryDir"], BINARY_DIR))
        # duplicate cache keys along the chain (each ancestor counted once, so a diamond is not a duplicate)
        owner = {}
        for n in [name] + sorted(set(ancestors), key=ancestors.index):
            for key in configure[n].get("cacheVariables", {}):
                if key in owner:
                    fail("dupkeys", "%s: cache key %s is defined in both %s and %s" % (name, key, owner[key], n))
                else:
                    owner[key] = n

    # pairing
    for name, p in configure.items():
        if p.get("hidden"):
            continue
        for kind, table in (("build", build), ("test", test)):
            q = table.get(name)
            if q is None:
                fail("pair", "configure preset %s has no %s preset of the same name" % (name, kind))
            elif q.get("configurePreset") != name:
                fail("pair", "%s preset %s points at %r" % (kind, name, q.get("configurePreset")))
    for kind, table in (("build", build), ("test", test)):
        for name, q in table.items():
            cp = q.get("configurePreset")
            if cp not in configure:
                fail("pair", "%s preset %s points at missing configure preset %r" % (kind, name, cp))
            elif configure[cp].get("hidden"):
                fail("pair", "%s preset %s points at hidden configure preset %s" % (kind, name, cp))
    for name, q in test.items():
        if (q.get("execution") or {}).get("noTestsAction") != "error":
            fail("pair", "test preset %s needs execution.noTestsAction = error" % name)
        if (q.get("output") or {}).get("outputOnFailure") is not True:
            fail("pair", "test preset %s needs output.outputOnFailure = true" % name)

    # workflows
    tables = {"configure": configure, "build": build, "test": test}
    for name, w in workflow.items():
        kinds = [s.get("type") for s in w.get("steps", [])]
        if kinds[:1] != ["configure"]:
            fail("workflow", "%s must start with a configure step" % name)
        for kind in ("build", "test"):
            if kind not in kinds:
                fail("workflow", "%s has no %s step" % (name, kind))
        for s in w.get("steps", []):
            table = tables.get(s.get("type"))
            if table is None or s.get("name") not in table:
                fail("workflow", "%s step %s %r does not exist" % (name, s.get("type"), s.get("name")))

    # windows and vcpkg conditions
    for name in configure:
        if name.startswith("win-"):
            for kind, table in (("configure", configure), ("build", build), ("test", test)):
                q = table.get(name)
                if q is not None and q.get("condition") != WINDOWS_COND:
                    fail("windows", "%s preset %s needs condition hostSystemName equals Windows" % (kind, name))
    if "vcpkg-debug" in configure:
        v = configure["vcpkg-debug"]
        if v.get("condition") != VCPKG_COND:
            fail("vcpkg", "vcpkg-debug needs condition $env{VCPKG_ROOT} notEquals \"\"")
        if v.get("toolchainFile") != VCPKG_TOOLCHAIN:
            fail("vcpkg", "vcpkg-debug toolchainFile must be %s" % VCPKG_TOOLCHAIN)
        for kind, table in (("build", build), ("test", test)):
            q = table.get("vcpkg-debug")
            if q is not None and q.get("condition") != VCPKG_COND:
                fail("vcpkg", "%s preset vcpkg-debug needs the VCPKG_ROOT condition" % kind)
    return errors


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = argv[1] if len(argv) > 1 else os.path.join(root, "CMakePresets.json")
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        print("presets-check: FAIL: schema: cannot read %s: %s" % (path, exc))
        return 1
    errors = check(doc)
    for e in errors:
        print("presets-check: FAIL: %s" % e)
    if errors:
        return 1
    print("presets-check: ok (%d configure, %d build, %d test, %d workflow presets)" % (
        len(doc.get("configurePresets", [])), len(doc.get("buildPresets", [])), len(doc.get("testPresets", [])),
        len(doc.get("workflowPresets", []))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
