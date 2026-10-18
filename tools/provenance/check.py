#!/usr/bin/env python3
"""provenance-check (WO-029, SL-GOV-006 F3): every built artifact has a valid, current provenance record.

  python3 -I tools/provenance/check.py [--root DIR] [--build-dir DIR] [--target NAME ...]

Records live in <build-dir>/provenance/<target>.prov.json (default build-dir: <root>/build/debug) and are written
by sl_provenance() (cmake/SlProvenance.cmake). Checked for sl_core, sl_sandbox and sl_tests (or the --target
names), plus any other *.prov.json found:
  R-PROV-LOAD            the record is missing or is not JSON without duplicate keys
  R-PROV-SCHEMA          the record does not validate against schemas/sl-provenance.schema.json
  R-PROV-TARGET          the record's target differs from its file name
  R-PROV-ARTIFACT-MISSING the artifact file the record names does not exist
  R-PROV-ARTIFACT-SHA    the artifact's sha256 differs from the record (rebuilt or altered after the record)
  R-PROV-INPUT-PATH      an input path is absolute or leaves the repository
  R-PROV-INPUT-MISSING   an input file no longer exists
  R-PROV-INPUT-STALE     an input's sha256 differs from the record (edited after the build; rebuild)
  R-PROV-ENV-HASH        environment_hash is not the sha256 of the canonical toolchain + parameters JSON
Exit 0 when clean, 1 with one line per finding. Stdlib only; reads files, runs nothing.
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import slib  # noqa: E402

DEFAULT_TARGETS = ("sl_core", "sl_sandbox", "sl_tests")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def environment_hash(record):
    """sha256 of {"parameters":..., "toolchain":...}, sorted keys, no spaces (the text SlProvenance.cmake hashes)."""
    text = json.dumps(
        {"parameters": record["parameters"], "toolchain": record["toolchain"]},
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    )
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def inside(root, rel):
    if os.path.isabs(rel):
        return False
    full = os.path.normpath(os.path.join(root, rel))
    return full == root or full.startswith(root + os.sep)


def check_record(path, name, root, build_dir, schema):
    """Return findings (rule, subject, message) for one record file."""
    try:
        record = slib.load_json(path)
    except slib.LoadError as exc:
        return [("R-PROV-LOAD", name, "no usable record: %s (run `just provenance-check` to build first)" % exc)]
    findings = []
    schema_errors = slib.validate(record, schema)
    for pointer, message in schema_errors:
        findings.append(("R-PROV-SCHEMA", name, "%s: %s" % (pointer, message)))
    if schema_errors:
        return findings
    if record["target"] != name:
        findings.append(("R-PROV-TARGET", name, "record names target %r" % record["target"]))
    artifact = record["artifact"]
    artifact_path = os.path.normpath(os.path.join(build_dir, artifact["path"]))
    if not inside(build_dir, artifact["path"]):
        findings.append(("R-PROV-INPUT-PATH", name, "artifact path %s leaves the build directory" % artifact["path"]))
    elif not os.path.isfile(artifact_path):
        findings.append(("R-PROV-ARTIFACT-MISSING", name, "artifact %s does not exist" % artifact["path"]))
    elif sha256_file(artifact_path) != artifact["sha256"]:
        findings.append(
            (
                "R-PROV-ARTIFACT-SHA",
                name,
                "artifact %s differs from the record (rebuild after the change)" % artifact["path"],
            )
        )
    for item in record["inputs"]:
        rel = item["path"]
        if not inside(root, rel):
            findings.append(("R-PROV-INPUT-PATH", name, "input %s is not a repository-relative path" % rel))
            continue
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            findings.append(("R-PROV-INPUT-MISSING", name, "input %s no longer exists" % rel))
        elif sha256_file(full) != item["sha256"]:
            message = "input %s changed after the build (stale record; rebuild)" % rel
            findings.append(("R-PROV-INPUT-STALE", name, message))
    expected = environment_hash(record)
    if record["environment_hash"] != expected:
        findings.append(
            (
                "R-PROV-ENV-HASH",
                name,
                "environment_hash %s, toolchain + parameters hash to %s" % (record["environment_hash"], expected),
            )
        )
    return findings


def run(root, build_dir, targets):
    schema = slib.load_schema("sl-provenance", os.path.join(root, "schemas"))
    prov_dir = os.path.join(build_dir, "provenance")
    names = list(targets)
    if os.path.isdir(prov_dir):
        for entry in sorted(os.listdir(prov_dir)):
            if entry.endswith(".prov.json") and entry[: -len(".prov.json")] not in names:
                names.append(entry[: -len(".prov.json")])
    findings = []
    for name in names:
        findings.extend(check_record(os.path.join(prov_dir, name + ".prov.json"), name, root, build_dir, schema))
    return names, findings


def main(argv):
    root = ROOT
    build_dir = None
    targets = []
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg in ("--root", "--build-dir", "--target") and args:
            value = args.pop(0)
            if arg == "--root":
                root = os.path.abspath(value)
            elif arg == "--build-dir":
                build_dir = os.path.abspath(value)
            else:
                targets.append(value)
        else:
            print("usage: check.py [--root DIR] [--build-dir DIR] [--target NAME ...]", file=sys.stderr)
            return 2
    build_dir = build_dir or os.path.join(root, "build", "debug")
    names, findings = run(root, build_dir, targets or DEFAULT_TARGETS)
    for rule, subject, message in findings:
        print("%s: %s: %s" % (rule, subject, message))
    if findings:
        print("provenance-check: FAIL (%d finding(s))" % len(findings))
        return 1
    where = os.path.relpath(os.path.join(build_dir, "provenance"), root)
    print("provenance-check: ok (%d records in %s)" % (len(names), where))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
