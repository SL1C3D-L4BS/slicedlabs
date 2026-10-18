#!/usr/bin/env python3
"""Fixture manifest gate (WO-027, SL-EP-052). Stdlib only, no subprocess, no network (ADR-0004).

Run with `python3 -I tools/testing/fixtures.py lock|check [--root DIR]`.

tests/fixtures/MANIFEST.json lists every file under tests/fixtures (except MANIFEST.json itself) with its sha256:
  {"schema": "sl-fixtures/1", "files": {"deps/cpm/fixture.json": {"sha256": "...", "bytes": 123}, ...}}

  lock    rewrites the manifest from the files on disk (the only way a fixture change is accepted)
  check   fails (exit 1) on any finding; each prints as `RULE path: message`

Rule ids:
  R-FIX-NOMANIFEST  MANIFEST.json is missing
  R-FIX-SCHEMA      MANIFEST.json is not valid JSON or does not have the expected shape
  R-FIX-MISSING     a manifested file is not on disk
  R-FIX-DRIFT       a file's sha256 differs from the manifest (edit the fixture, then `lock`, and review the diff)
  R-FIX-UNLISTED    a file on disk is not in the manifest (new fixture: run `lock`)
  R-FIX-SYMLINK     a symlink under tests/fixtures (fixtures are plain files)

Generated outputs never live under tests/fixtures (the cpm tarball is rebuilt under build/ at test time).
"""

import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCHEMA = "sl-fixtures/1"
MANIFEST = "MANIFEST.json"
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def scan(fixtures_dir):
    """Return ({relative posix path: (sha256, bytes)}, [symlink relative paths])."""
    files = {}
    links = []
    for base, dirs, names in os.walk(fixtures_dir):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, fixtures_dir).replace(os.sep, "/")
            if rel == MANIFEST:
                continue
            if os.path.islink(full):
                links.append(rel)
                continue
            files[rel] = (_sha256(full), os.path.getsize(full))
    for base, dirs, _names in os.walk(fixtures_dir):
        for name in dirs:
            full = os.path.join(base, name)
            if os.path.islink(full):
                links.append(os.path.relpath(full, fixtures_dir).replace(os.sep, "/"))
    return files, sorted(links)


def render(files):
    entries = {rel: {"sha256": sha, "bytes": size} for rel, (sha, size) in sorted(files.items())}
    return json.dumps({"schema": SCHEMA, "files": entries}, indent=2, ensure_ascii=False) + "\n"


def lock(fixtures_dir):
    files, links = scan(fixtures_dir)
    if links:
        return ["R-FIX-SYMLINK %s: fixtures must be plain files" % rel for rel in links]
    with open(os.path.join(fixtures_dir, MANIFEST), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(render(files))
    print("fixtures-lock: %d file(s) manifested" % len(files))
    return []


def _load(path):
    """Return (entries or None, findings)."""
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError:
        return None, ["R-FIX-NOMANIFEST %s: missing; run `python3 -I tools/testing/fixtures.py lock`" % MANIFEST]
    except (OSError, ValueError) as err:
        return None, ["R-FIX-SCHEMA %s: not valid JSON (%s)" % (MANIFEST, err)]
    if not isinstance(data, dict) or data.get("schema") != SCHEMA or not isinstance(data.get("files"), dict):
        return None, ["R-FIX-SCHEMA %s: expected {\"schema\": \"%s\", \"files\": {...}}" % (MANIFEST, SCHEMA)]
    findings = []
    for rel, entry in data["files"].items():
        ok = isinstance(entry, dict) and isinstance(entry.get("sha256"), str) and SHA_RE.match(entry["sha256"])
        if not ok or rel == MANIFEST or rel.startswith("/") or ".." in rel.split("/"):
            findings.append("R-FIX-SCHEMA %s: bad entry '%s'" % (MANIFEST, rel))
    if findings:
        return None, findings
    return data["files"], []


def check(fixtures_dir):
    entries, findings = _load(os.path.join(fixtures_dir, MANIFEST))
    if entries is None:
        return findings
    files, links = scan(fixtures_dir)
    findings += ["R-FIX-SYMLINK %s: fixtures must be plain files" % rel for rel in links]
    for rel in sorted(entries):
        if rel not in files:
            findings.append("R-FIX-MISSING %s: listed in %s but not on disk" % (rel, MANIFEST))
        elif files[rel][0] != entries[rel]["sha256"]:
            got, want = files[rel][0][:12], entries[rel]["sha256"][:12]
            findings.append("R-FIX-DRIFT %s: sha256 %s, manifest has %s" % (rel, got, want))
    for rel in sorted(files):
        if rel not in entries:
            findings.append("R-FIX-UNLISTED %s: not in %s; run `lock` to add it" % (rel, MANIFEST))
    if not findings:
        print("fixtures-check: ok (%d file(s))" % len(files))
    return findings


def main(argv):
    args = list(argv)
    root = ROOT
    if "--root" in args:
        i = args.index("--root")
        if i + 1 >= len(args):
            print("usage: fixtures.py lock|check [--root DIR]", file=sys.stderr)
            return 2
        root = os.path.abspath(args[i + 1])
        del args[i : i + 2]
    if len(args) != 1 or args[0] not in ("lock", "check"):
        print("usage: fixtures.py lock|check [--root DIR]", file=sys.stderr)
        return 2
    fixtures_dir = os.path.join(root, "tests", "fixtures")
    if not os.path.isdir(fixtures_dir):
        print("R-FIX-NOMANIFEST tests/fixtures: directory missing", file=sys.stderr)
        return 1
    findings = lock(fixtures_dir) if args[0] == "lock" else check(fixtures_dir)
    for line in findings:
        print(line, file=sys.stderr)
    if findings:
        print("fixtures-%s: FAIL (%d finding(s))" % (args[0], len(findings)), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
