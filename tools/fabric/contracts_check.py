"""fabric-contracts-check: bind every FAB-INV id of SL-ARCH-001/002 to one owning work order and one test.

Reads the `invariants` and `retired` blocks (sl-data) of docs/arch/SL-ARCH-00{1,2}-*.md, then scans
tests/unit/fabric/**/*.cpp and tests/integration/fabric/**/*.cpp for TEST_CASE("FAB-INV-<AREA>-<nn>: ...") and
tools/fabric/test_*.py for `def test_fab_inv_<area>_<nn>_*`. Rules (each error line names the rule and the id):

  R-FAB-SHAPE         an entry lacks one of id, rule, text, test, wo, verified_by (or has a bad value or an extra key)
  R-FAB-DUP           an id appears twice across both documents (invariants and retired together)
  R-FAB-RULE          the rule id is not a `- ARCHn-Rnn:` line of the entry's own document
  R-FAB-WO            the owning work order has no work-orders/WO-nnn.md file
  R-FAB-ORPHAN-TEST   a test names an id absent from the documents, or listed as retired
  R-FAB-LOCATION      a test names the id but lives outside the entry's `test` file
  R-FAB-RECIPE        verified_by (or a `just` test) is not a justfile recipe; a stub is pending, an error in strict
  R-FAB-MISSING-TEST  no test names the id yet: pending, an error in strict

Modes: default treats R-FAB-MISSING-TEST and stub recipes as pending (printed, exit 0); --strict makes them errors and
requires `test: just <recipe>` entries to be implemented recipes. The output ends with
`invariants N bound B pending P`. Stdlib only, no subprocess (ADR-0004); run as
`python3 -I tools/fabric/contracts_check.py [--strict] [--root DIR]`.
"""

import os
import re
import sys

_GOV = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "governance")
sys.path.insert(0, _GOV)
import cmd_constitution_check  # noqa: E402
import cmd_contracts_pending  # noqa: E402
import slgov  # noqa: E402

KEYS = ("id", "rule", "text", "test", "wo", "verified_by")
ID_RE = re.compile(r"^FAB-INV-[A-Z0-9]+-\d\d$")
RULE_RE = re.compile(r"^ARCH\d-R\d\d+$")
WO_RE = re.compile(r"^WO-\d{3}$")
JUST_RE = re.compile(r"^just\s+([A-Za-z_][\w-]*)(?:\s.*)?$")
RULE_LINE = re.compile(r"^- (ARCH\d-R\d+):", re.MULTILINE)
CPP_TEST = re.compile(r'^\s*TEST_CASE\w*\(\s*"(FAB-INV-[A-Z0-9]+-\d\d)\s*:', re.MULTILINE)
PY_TEST = re.compile(r"^\s*def test_fab_inv_([a-z0-9]+)_(\d\d)(?:_\w*)?\(", re.MULTILINE)
CPP_DIRS = ("tests/unit/fabric", "tests/integration/fabric")
PY_DIR = "tools/fabric"
TEST_PATH = re.compile(r"^(?:tests/(?:unit|integration)/fabric/[\w./-]+\.cpp|tools/fabric/test_\w+\.py)$")
DOCS = ((1, "SL-ARCH-001-"), (2, "SL-ARCH-002-"))
WIDTH = 118


def _read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except (OSError, UnicodeDecodeError):
        return None


def load_documents(root, errors):
    """Return a list of (doc number, relative path, rule ids, invariants, retired) for the two ARCH documents."""
    docs = []
    arch = os.path.join(root, "docs", "arch")
    names = sorted(os.listdir(arch)) if os.path.isdir(arch) else []
    for number, prefix in DOCS:
        found = [n for n in names if n.startswith(prefix) and n.endswith(".md")]
        if not found:
            errors.append("R-FAB-SHAPE: docs/arch/%s*.md is missing (document %d of 2)" % (prefix, number))
            continue
        rel = "docs/arch/" + found[0]
        text = _read(os.path.join(arch, found[0]))
        if text is None:
            errors.append("%s: R-FAB-SHAPE: unreadable" % rel)
            continue
        blocks, bad = cmd_constitution_check.sl_data_blocks(text)
        errors.extend("%s: R-FAB-SHAPE: %s" % (rel, b) for b in bad)
        holders = [b for b in blocks if isinstance(b, dict) and "invariants" in b]
        if len(holders) != 1:
            errors.append("%s: R-FAB-SHAPE: expected exactly one sl-data block with `invariants`, found %d"
                          % (rel, len(holders)))
            continue
        invariants, retired = holders[0]["invariants"], holders[0].get("retired")
        if not isinstance(invariants, list) or not isinstance(retired, list):
            errors.append("%s: R-FAB-SHAPE: `invariants` and `retired` must both be lists" % rel)
            continue
        docs.append((number, rel, set(RULE_LINE.findall(text)), invariants, retired))
    return docs


def scan_tests(root):
    """Map invariant id -> sorted relative paths of the tests that name it."""
    named = {}

    def add(fid, rel):
        named.setdefault(fid, set()).add(rel)

    for rel_dir in CPP_DIRS:
        top = os.path.join(root, *rel_dir.split("/"))
        for here, dirs, files in os.walk(top):
            dirs.sort()
            for name in sorted(files):
                if not name.endswith(".cpp"):
                    continue
                full = os.path.join(here, name)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                for fid in CPP_TEST.findall(_read(full) or ""):
                    add(fid, rel)
    py_top = os.path.join(root, *PY_DIR.split("/"))
    for name in sorted(os.listdir(py_top)) if os.path.isdir(py_top) else []:
        if name.startswith("test_") and name.endswith(".py"):
            for area, number in PY_TEST.findall(_read(os.path.join(py_top, name)) or ""):
                add("FAB-INV-%s-%s" % (area.upper(), number), "%s/%s" % (PY_DIR, name))
    return {k: sorted(v) for k, v in named.items()}


def _shape(entry, rel, index):
    """Return a list of shape error strings for one invariant entry."""
    label = entry.get("id") if isinstance(entry, dict) and isinstance(entry.get("id"), str) else "entry %d" % index
    if not isinstance(entry, dict):
        return ["%s: R-FAB-SHAPE: %s is not an object" % (rel, label)]
    errs = []
    for key in KEYS:
        if not isinstance(entry.get(key), str) or not entry[key].strip():
            errs.append("missing or empty `%s`" % key)
    for key in sorted(set(entry) - set(KEYS)):
        errs.append("unknown key `%s`" % key)
    if not errs:
        for key, rx, what in (("id", ID_RE, "FAB-INV-<AREA>-<nn>"), ("rule", RULE_RE, "ARCHn-Rnn"),
                              ("wo", WO_RE, "WO-nnn")):
            if not rx.match(entry[key]):
                errs.append("`%s` %r is not %s" % (key, entry[key], what))
        if not (JUST_RE.match(entry["verified_by"])):
            errs.append("`verified_by` %r is not `just <recipe>`" % entry["verified_by"])
        if not (JUST_RE.match(entry["test"]) or TEST_PATH.match(entry["test"])):
            errs.append("`test` %r is neither `just <recipe>` nor a tests/{unit,integration}/fabric .cpp or "
                        "tools/fabric/test_*.py path" % entry["test"])
    return ["%s: R-FAB-SHAPE: %s: %s" % (rel, label, e) for e in errs]


def check(root, strict=False):
    """Return (errors, pending, counts). pending is a list of (rule, wo, id, detail); counts is (N, bound, pending)."""
    errors, pending = [], []
    docs = load_documents(root, errors)
    recipes = cmd_contracts_pending.recipe_state(root)
    named = scan_tests(root)
    seen, retired_ids, entries = {}, set(), []
    for number, rel, rules, invariants, retired in docs:
        for i, entry in enumerate(invariants):
            bad = _shape(entry, rel, i)
            errors.extend(bad)
            if bad:
                continue
            entries.append((rel, rules, entry))
            seen.setdefault(entry["id"], []).append(rel)
        for i, item in enumerate(retired):
            rid = item if isinstance(item, str) else item.get("id") if isinstance(item, dict) else None
            if not isinstance(rid, str) or not ID_RE.match(rid):
                errors.append("%s: R-FAB-SHAPE: retired entry %d has no valid FAB-INV id" % (rel, i))
                continue
            retired_ids.add(rid)
            seen.setdefault(rid, []).append(rel + " (retired)")
    for fid in sorted(seen):
        if len(seen[fid]) > 1:
            errors.append("R-FAB-DUP: %s appears %d times: %s" % (fid, len(seen[fid]), ", ".join(seen[fid])))
    for fid in sorted(named):
        if fid not in seen or fid in retired_ids:
            errors.append("R-FAB-ORPHAN-TEST: %s is %s but named by %s"
                          % (fid, "retired" if fid in retired_ids else "not in any ARCH document",
                             ", ".join(named[fid])))
    pending_ids = set()
    for rel, rules, entry in entries:
        fid = entry["id"]
        if entry["rule"] not in rules:
            errors.append("%s: R-FAB-RULE: %s names rule %s, which is not a `- %s-Rnn:` line of this document"
                          % (rel, fid, entry["rule"], entry["rule"].split("-")[0]))
        if not os.path.isfile(os.path.join(root, "work-orders", entry["wo"] + ".md")):
            errors.append("%s: R-FAB-WO: %s names %s, which has no work-orders/%s.md"
                          % (rel, fid, entry["wo"], entry["wo"]))
        verifier = JUST_RE.match(entry["verified_by"]).group(1)
        if verifier not in recipes:
            errors.append("%s: R-FAB-RECIPE: %s verified_by `%s` is not a justfile recipe" % (rel, fid, verifier))
        elif recipes[verifier][0]:
            pending_ids.add(fid)
            pending.append(("R-FAB-RECIPE", entry["wo"], fid, "verified_by recipe %s is a stub" % verifier))
        by_recipe = JUST_RE.match(entry["test"])
        homes = named.get(fid, [])
        for other in homes:
            if other != entry["test"]:
                errors.append("R-FAB-LOCATION: %s is named by %s but its `test` is %s" % (fid, other, entry["test"]))
        if by_recipe:
            test_recipe = by_recipe.group(1)
            if test_recipe not in recipes:
                errors.append("%s: R-FAB-RECIPE: %s test `%s` is not a justfile recipe" % (rel, fid, test_recipe))
            elif recipes[test_recipe][0]:
                pending_ids.add(fid)
                pending.append(("R-FAB-MISSING-TEST", entry["wo"], fid, "test recipe %s is a stub" % test_recipe))
        elif entry["test"] not in homes:
            pending_ids.add(fid)
            pending.append(("R-FAB-MISSING-TEST", entry["wo"], fid, "no test yet in " + entry["test"]))
    if strict:
        errors.extend("%s: %s: %s" % (rule, fid, detail) for rule, _wo, fid, detail in pending)
    return errors, pending, (len(entries), len(entries) - len(pending_ids), len(pending_ids))


def _group(pending):
    """Group pending items by (rule, wo): {(rule, wo): [ids]}."""
    out = {}
    for rule, wo, fid, _detail in pending:
        out.setdefault((rule, wo), []).append(fid)
    return out


def _wrap(prefix, ids):
    lines, cur = [], prefix
    for fid in ids:
        if len(cur) + len(fid) + 1 > WIDTH and cur.strip() != prefix.strip():
            lines.append(cur)
            cur = "    "
        cur += (" " if not cur.endswith(" ") else "") + fid
    lines.append(cur)
    return lines


def main(argv):
    root, strict, args = slgov.ROOT, False, argv[1:]
    while args:
        if args[0] == "--strict":
            strict = True
            args = args[1:]
        elif args[0] == "--root" and len(args) >= 2:
            root = os.path.abspath(args[1])
            args = args[2:]
        else:
            print("usage: contracts_check.py [--strict] [--root DIR]", file=sys.stderr)
            return 2
    errors, pending, (total, bound, waiting) = check(root, strict)
    for e in errors:
        print(e)
    if not strict:
        for (rule, wo), ids in sorted(_group(pending).items()):
            for line in _wrap("pending %s %s (%d):" % (rule, wo, len(ids)), ids):
                print(line)
    name = "fabric-contracts-strict" if strict else "fabric-contracts-check"
    print("%s: %s" % (name, "FAIL (%d error(s))" % len(errors) if errors else "ok"))
    print("invariants %d bound %d pending %d" % (total, bound, waiting))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
