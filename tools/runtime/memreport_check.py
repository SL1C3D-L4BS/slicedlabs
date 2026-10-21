"""runtime-memreport-check: validate sl-runtime-memreport/1 reports (WO-085, SL-ARCH-003 ARCH3-R052 to R054).

Each file is validated with the subset validator of tools/governance/slib.py against
schemas/runtime/memreport.schema.json, then for the semantics the schema cannot state: per tag live_bytes <= peak_bytes
and frees <= allocs, fragmentation_permille in 0..1000, tags sorted by name (strictly: no duplicates) and leaks sorted
by (tag, site, size). With --no-leaks every leak entry is an error naming its tag, site and size.

Without file arguments the reports are build/runtime/test/memreport.json (written by `just runtime-test diag`) and
build/runtime/gate/memreport.json, whichever exist; none existing is exit 1. Output lines are
`memreport-check: <file>: <json pointer>: <message>` per finding and `memreport-check: <file>: OK`; the exit status is 0
when every file passes, 1 on any finding, 2 on usage. Stdlib only, no subprocess, no network (ADR-0004):
`python3 -I tools/runtime/memreport_check.py [file...] [--no-leaks]`.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import slib  # noqa: E402

SCHEMA_PATH = os.path.join("schemas", "runtime", "memreport.schema.json")
DEFAULT_FILES = (
    os.path.join("build", "runtime", "test", "memreport.json"),
    os.path.join("build", "runtime", "gate", "memreport.json"),
)
_COUNTERS = ("live_bytes", "live_blocks", "peak_bytes", "allocs", "frees", "failures")


def load_schema(root=ROOT):
    return slib.load_json(os.path.join(root, SCHEMA_PATH))


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _objects(report, key):
    """The (index, object) pairs of array `key`, skipping what the schema check already rejects."""
    items = report.get(key) if isinstance(report, dict) else None
    if not isinstance(items, list):
        return []
    return [(i, item) for i, item in enumerate(items) if isinstance(item, dict)]


def semantic_findings(report, no_leaks=False):
    """The (pointer, message) findings beyond the schema."""
    out = []
    tag_names = []
    for i, tag in _objects(report, "tags"):
        if all(_is_int(tag.get(k)) for k in _COUNTERS):
            if tag["live_bytes"] > tag["peak_bytes"]:
                out.append(("/tags/%d/live_bytes" % i, "live_bytes %d above peak_bytes %d"
                            % (tag["live_bytes"], tag["peak_bytes"])))
            if tag["frees"] > tag["allocs"]:
                out.append(("/tags/%d/frees" % i, "frees %d above allocs %d" % (tag["frees"], tag["allocs"])))
        if isinstance(tag.get("tag"), str):
            tag_names.append((i, tag["tag"]))
    for (_, before), (i, name) in zip(tag_names, tag_names[1:]):
        if not before < name:
            out.append(("/tags/%d/tag" % i, "tags are not sorted by name: %r after %r" % (name, before)))
    for i, alloc in _objects(report, "allocators"):
        permille = alloc.get("fragmentation_permille")
        if _is_int(permille) and not 0 <= permille <= 1000:
            out.append(("/allocators/%d/fragmentation_permille" % i, "%d is outside 0..1000" % permille))
    leaks = []
    for i, leak in _objects(report, "leaks"):
        key = (leak.get("tag"), leak.get("site"), leak.get("size"))
        if isinstance(key[0], str) and isinstance(key[1], str) and _is_int(key[2]):
            leaks.append((i, key))
        if no_leaks:
            out.append(("/leaks/%d" % i, "leak of %s bytes, tag %s, site %s" % (key[2], key[0], key[1])))
    for (_, before), (i, key) in zip(leaks, leaks[1:]):
        if key < before:
            out.append(("/leaks/%d" % i, "leaks are not sorted by (tag, site, size): %r after %r"
                        % (list(key), list(before))))
    return out


def check_report(report, schema, no_leaks=False):
    """All findings of one parsed report: schema first, then semantics."""
    return [(p or "/", m) for p, m in slib.validate(report, schema)] + semantic_findings(report, no_leaks)


def check_file(path, schema, no_leaks=False):
    try:
        report = slib.load_json(path)
    except slib.LoadError as exc:
        return [("/", str(exc))]
    return check_report(report, schema, no_leaks)


def main(argv, root=ROOT, out=None):
    out = out or sys.stdout
    no_leaks = False
    files = []
    for arg in argv:
        if arg == "--no-leaks":
            no_leaks = True
        elif arg.startswith("-"):
            print("usage: memreport_check.py [file...] [--no-leaks]", file=out)
            return 2
        else:
            files.append(arg)
    if not files:
        files = [os.path.join(root, f) for f in DEFAULT_FILES if os.path.isfile(os.path.join(root, f))]
        if not files:
            print("memreport-check: no report found (%s); run `just runtime-test diag`" % ", ".join(DEFAULT_FILES),
                  file=out)
            return 1
    try:
        schema = load_schema(root)
    except slib.LoadError as exc:
        print("memreport-check: %s" % exc, file=out)
        return 1
    status = 0
    for path in files:
        findings = check_file(path, schema, no_leaks)
        shown = os.path.relpath(path, root) if os.path.isabs(path) else path
        for pointer, message in findings:
            print("memreport-check: %s: %s: %s" % (shown, pointer, message), file=out)
        if findings:
            status = 1
        else:
            print("memreport-check: %s: OK" % shown, file=out)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
