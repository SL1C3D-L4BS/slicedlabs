"""runtime-contracts: bind every RT-INV id of SL-ARCH-003 to one owning work order and one test (WO-077, ARCH3-R014).

Reads the single `invariants` block (sl-data) of docs/arch/SL-ARCH-003-*.md, then scans tests/unit/runtime/**/*.cpp and
tests/integration/runtime/**/*.cpp for TEST_CASE("RT-INV-<AREA>-<nn>: ...") and tools/runtime/test_*.py for
`def test_rt_inv_<area>_<nn>_*`. Rules (each error line names the rule and the id):

  R-RT-SHAPE         a row lacks one of id, rule, text, test, wo, verified_by, has another key or a bad value (the id
                     is RT-INV-<AREA>-<nn> with AREA from the document's list), or the document is missing or has not
                     exactly one `invariants` block
  R-RT-DUP           an id appears twice
  R-RT-RULE          the rule is not a `- ARCH3-Rnnn:` line of the document
  R-RT-WO            the owner has no work-orders/WO-nnn.md, that file does not list the id under its owned
                     invariants, or a work order lists an id the table gives to another owner (both ways)
  R-RT-ORPHAN-TEST   a test names an id absent from the table
  R-RT-LOCATION      a test names the id but lives outside the row's `test` file
  R-RT-AREA          the C++ test file of a row does not start with the lower-case area (stress_* files excepted)
  R-RT-PER-WO        a work order owns more than 8 rows
  R-RT-RECIPE        verified_by is not a justfile recipe; a stub recipe, and a `dispatch` recipe whose
                     scripts/runtime/<name>.sh is still the two-line cmd_stub.py stub, are pending
  R-RT-MISSING-TEST  no test names the id yet in the row's file: pending

Modes: the default prints pending items and exits 0; --strict (recipe runtime-contracts-strict) makes every pending
item an error. The output ends with `invariants N bound B pending P`. Stdlib only, no subprocess (ADR-0004); run as
`python3 -I tools/runtime/contracts_check.py [--strict] [--root DIR]`.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import cmd_constitution_check  # noqa: E402
import cmd_contracts_pending  # noqa: E402

KEYS = ("id", "rule", "text", "test", "wo", "verified_by")
DEFAULT_AREAS = ("ERR", "FLT", "LINT", "PLAT", "MEM", "SYNC", "ALLOC", "JOB", "ENT", "CLOCK", "DET", "DIAG", "STEAL",
                 "REFL", "STORE", "FRAME", "QUERY", "CMD", "EVT", "CHG", "SCHED", "SNAP", "LIFE", "BND", "STRESS",
                 "GATE")
ID_RE = re.compile(r"^RT-INV-([A-Z]+)-(\d\d)$")
RULE_RE = re.compile(r"^ARCH3-R\d{3}$")
WO_RE = re.compile(r"^WO-\d{3}$")
JUST_RE = re.compile(r"^just\s+([A-Za-z_][\w-]*)(?:\s.*)?$")
RULE_LINE = re.compile(r"^- (ARCH3-R\d+):", re.MULTILINE)
AREA_LIST = re.compile(r"AREA one of ([A-Z\s]+?),")
CPP_TEST = re.compile(r'^\s*TEST_CASE\w*\(\s*"(RT-INV-[A-Z]+-\d\d)\s*:', re.MULTILINE)
PY_TEST = re.compile(r"^\s*def test_rt_inv_([a-z]+)_(\d\d)(?:_\w*)?\(", re.MULTILINE)
WO_LISTS = re.compile(r"^- (RT-INV-[A-Z]+-\d\d)\b", re.MULTILINE)
CPP_DIRS = ("tests/unit/runtime", "tests/integration/runtime")
PY_DIR = "tools/runtime"
TEST_PATH = re.compile(r"^(?:tests/(?:unit|integration)/runtime/[\w./-]+\.cpp|tools/runtime/test_\w+\.py)$")
MAX_PER_WO = 8
WIDTH = 118


def _read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except (OSError, UnicodeDecodeError):
        return None


def load_document(root, errors):
    """Return (relative path, rule ids, invariants, areas) of SL-ARCH-003, or None after recording the error."""
    arch = os.path.join(root, "docs", "arch")
    names = sorted(n for n in os.listdir(arch) if n.startswith("SL-ARCH-003-") and n.endswith(".md")) \
        if os.path.isdir(arch) else []
    if not names:
        errors.append("R-RT-SHAPE: docs/arch/SL-ARCH-003-*.md is missing")
        return None
    rel = "docs/arch/" + names[0]
    text = _read(os.path.join(arch, names[0]))
    if text is None:
        errors.append("%s: R-RT-SHAPE: unreadable" % rel)
        return None
    blocks, bad = cmd_constitution_check.sl_data_blocks(text)
    errors.extend("%s: R-RT-SHAPE: %s" % (rel, b) for b in bad)
    holders = [b for b in blocks if isinstance(b, dict) and "invariants" in b]
    if len(holders) != 1:
        errors.append("%s: R-RT-SHAPE: expected exactly one sl-data block with `invariants`, found %d"
                      % (rel, len(holders)))
        return None
    invariants = holders[0]["invariants"]
    if not isinstance(invariants, list):
        errors.append("%s: R-RT-SHAPE: `invariants` must be a list" % rel)
        return None
    listed = AREA_LIST.search(text)
    areas = tuple(listed.group(1).split()) if listed else DEFAULT_AREAS
    return rel, set(RULE_LINE.findall(text)), invariants, areas


def scan_tests(root):
    """Map invariant id -> sorted relative paths of the tests that name it."""
    named = {}

    def add(rid, rel):
        named.setdefault(rid, set()).add(rel)

    for rel_dir in CPP_DIRS:
        top = os.path.join(root, *rel_dir.split("/"))
        for here, dirs, files in os.walk(top):
            dirs.sort()
            for name in sorted(files):
                if name.endswith(".cpp"):
                    full = os.path.join(here, name)
                    rel = os.path.relpath(full, root).replace(os.sep, "/")
                    for rid in CPP_TEST.findall(_read(full) or ""):
                        add(rid, rel)
    top = os.path.join(root, *PY_DIR.split("/"))
    for name in sorted(os.listdir(top)) if os.path.isdir(top) else []:
        if name.startswith("test_") and name.endswith(".py"):
            for area, number in PY_TEST.findall(_read(os.path.join(top, name)) or ""):
                add("RT-INV-%s-%s" % (area.upper(), number), "%s/%s" % (PY_DIR, name))
    return {k: sorted(v) for k, v in named.items()}


def work_order_lists(root):
    """Map work order id -> set of RT-INV ids its file lists as owned (`- RT-INV-...` lines)."""
    lists = {}
    top = os.path.join(root, "work-orders")
    for name in sorted(os.listdir(top)) if os.path.isdir(top) else []:
        if re.match(r"^WO-\d{3}\.md$", name):
            lists[name[:-3]] = set(WO_LISTS.findall(_read(os.path.join(top, name)) or ""))
    return lists


def _shape(entry, rel, index, areas):
    """Return a list of shape error strings for one invariant row."""
    label = entry.get("id") if isinstance(entry, dict) and isinstance(entry.get("id"), str) else "row %d" % index
    if not isinstance(entry, dict):
        return ["%s: R-RT-SHAPE: %s is not an object" % (rel, label)]
    errs = []
    for key in KEYS:
        if not isinstance(entry.get(key), str) or not entry[key].strip():
            errs.append("missing or empty `%s`" % key)
    for key in sorted(set(entry) - set(KEYS)):
        errs.append("unknown key `%s`" % key)
    if not errs:
        m = ID_RE.match(entry["id"])
        if not m:
            errs.append("`id` %r is not RT-INV-<AREA>-<nn>" % entry["id"])
        elif m.group(1) not in areas:
            errs.append("`id` %r: area %s is not in the SL-ARCH-003 list (%s)" % (entry["id"], m.group(1),
                                                                                " ".join(areas)))
        for key, rx, what in (("rule", RULE_RE, "ARCH3-Rnnn"), ("wo", WO_RE, "WO-nnn"),
                              ("verified_by", JUST_RE, "`just <recipe>`")):
            if not rx.match(entry[key]):
                errs.append("`%s` %r is not %s" % (key, entry[key], what))
        if not TEST_PATH.match(entry["test"]):
            errs.append("`test` %r is not a tests/{unit,integration}/runtime .cpp or tools/runtime/test_*.py path"
                        % entry["test"])
    return ["%s: R-RT-SHAPE: %s: %s" % (rel, label, e) for e in errs]


def check(root, strict=False):
    """Return (errors, pending, counts). pending is a list of (rule, wo, id, detail); counts is (N, bound, pending)."""
    errors, pending = [], []
    doc = load_document(root, errors)
    if doc is None:
        return errors, pending, (0, 0, 0)
    rel, rules, invariants, areas = doc
    recipes = cmd_contracts_pending.recipe_state(root)
    named = scan_tests(root)
    owned = work_order_lists(root)
    seen, entries = {}, []
    for i, entry in enumerate(invariants):
        bad = _shape(entry, rel, i, areas)
        errors.extend(bad)
        if bad:
            continue
        entries.append(entry)
        seen.setdefault(entry["id"], []).append(entry)
    for rid in sorted(seen):
        if len(seen[rid]) > 1:
            errors.append("R-RT-DUP: %s appears %d times in the invariants table" % (rid, len(seen[rid])))
    for rid in sorted(named):
        if rid not in seen:
            errors.append("R-RT-ORPHAN-TEST: %s is not in the invariants table but named by %s"
                          % (rid, ", ".join(named[rid])))
    per_wo = {}
    pending_ids = set()
    for entry in entries:
        rid, wo = entry["id"], entry["wo"]
        per_wo[wo] = per_wo.get(wo, 0) + 1
        if entry["rule"] not in rules:
            errors.append("%s: R-RT-RULE: %s names rule %s, which is not a `- ARCH3-Rnnn:` line of this document"
                          % (rel, rid, entry["rule"]))
        if not os.path.isfile(os.path.join(root, "work-orders", wo + ".md")):
            errors.append("%s: R-RT-WO: %s names %s, which has no work-orders/%s.md" % (rel, rid, wo, wo))
        elif rid not in owned.get(wo, ()):
            errors.append("%s: R-RT-WO: %s names %s, whose work order does not list it among its invariants"
                          % (rel, rid, wo))
        base = entry["test"].rsplit("/", 1)[-1]
        area = ID_RE.match(rid).group(1).lower()
        if entry["test"].endswith(".cpp") and not (base.startswith(area) or base.startswith("stress")):
            errors.append("%s: R-RT-AREA: %s test file %s does not start with the area `%s`" % (rel, rid, base, area))
        verifier = JUST_RE.match(entry["verified_by"]).group(1)
        if verifier not in recipes:
            errors.append("%s: R-RT-RECIPE: %s verified_by `%s` is not a justfile recipe" % (rel, rid, verifier))
        elif recipes[verifier][0]:
            pending_ids.add(rid)
            pending.append(("R-RT-RECIPE", wo, rid, "verified_by recipe %s is a stub" % verifier))
        homes = named.get(rid, [])
        for other in homes:
            if other != entry["test"]:
                errors.append("R-RT-LOCATION: %s is named by %s but its `test` is %s" % (rid, other, entry["test"]))
        if entry["test"] not in homes:
            pending_ids.add(rid)
            pending.append(("R-RT-MISSING-TEST", wo, rid, "no test yet in " + entry["test"]))
    for wo in sorted(per_wo):
        if per_wo[wo] > MAX_PER_WO:
            errors.append("R-RT-PER-WO: %s owns %d invariants, at most %d" % (wo, per_wo[wo], MAX_PER_WO))
    table = {e["id"]: e["wo"] for e in entries}
    for wo in sorted(owned):
        for rid in sorted(owned[wo]):
            if rid not in table:
                errors.append("R-RT-WO: work-orders/%s.md lists %s, which is not in the invariants table" % (wo, rid))
            elif table[rid] != wo:
                errors.append("R-RT-WO: work-orders/%s.md lists %s, which the table gives to %s"
                              % (wo, rid, table[rid]))
    if strict:
        errors.extend("%s: %s: %s" % (rule, rid, detail) for rule, _wo, rid, detail in pending)
    return errors, pending, (len(entries), len(entries) - len(pending_ids), len(pending_ids))


def _group(pending):
    out = {}
    for rule, wo, rid, _detail in pending:
        out.setdefault((rule, wo), []).append(rid)
    return out


def _wrap(prefix, ids):
    lines, cur = [], prefix
    for rid in ids:
        if len(cur) + len(rid) + 1 > WIDTH and cur.strip() != prefix.strip():
            lines.append(cur)
            cur = "    "
        cur += (" " if not cur.endswith(" ") else "") + rid
    lines.append(cur)
    return lines


def main(argv):
    root, strict, args = ROOT, False, argv[1:]
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
    name = "runtime-contracts-strict" if strict else "runtime-contracts"
    print("%s: %s" % (name, "FAIL (%d error(s))" % len(errors) if errors else "ok"))
    print("invariants %d bound %d pending %d" % (total, bound, waiting))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
