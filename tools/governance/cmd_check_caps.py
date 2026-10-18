"""capability-check: the capability register SL-REG-004 and the documents SL-GOV-003 and SL-GOV-010 agree
with the master plan sections 2.2 (twelve subsystems) and 2.7 (Unreal parity matrix).

Rules (each failure line contains the rule id):
  R-CAP-SHAPE     the register is {schema "sl-reg-004/1", capabilities[]}; each capability has exactly the keys
                  id (CAP-<SS>-<NAME>), subsystem, boundary (core|module|plugin|external|hosted), interface_doc,
                  introduced_in, unreal_counterpart (string or null), src; ids are unique and agree with subsystem
  R-CAP-SUBSYS    every subsystem is 01-12; each of the 12 rows of section 2.2 has a capability with src
                  "SL-PLAN-001 §2.2"; the `subsystems` block of SL-GOV-003 equals the section 2.2 table
  R-CAP-DOC       every interface_doc is an SL-ARCH document registered in SL-REG-001
  R-CAP-EP        every introduced_in is an episode of SL-REG-002
  R-CAP-PARITY    every row of section 2.7 (counted from the plan) has exactly one capability with that
                  unreal_counterpart and src "SL-PLAN-001 §2.7 row N"; the `rows` block of SL-GOV-010 equals the
                  plan rows and names those capabilities; the benchmark policy block is present
  R-CAP-ONTOLOGY  SL-GOV-003 lists the five boundary classes of episode 006 and a non-empty extensibility list
  R-CAP-CLAIM     no "rivals", "beats" or "better than" in the register, SL-GOV-003 or SL-GOV-010, except inside
                  double quotes or inside a verbatim section 2.7 obligation (master plan section 6.3, rule 5)

The schemas/sl-reg-004.schema.json of WO-002 describes a different shape (SL-CAP-NNN ids), so the shape is checked
here (R-CAP-SHAPE) instead of by that schema. Stdlib only (ADR-0004); run with
`python3 -I cmd_check_caps.py [--root DIR]`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check as cc  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

PLAN_REL = cc.PLAN_REL
REGISTER_REL = "registry/SL-REG-004-capabilities.json"
DOCS_REL = "registry/SL-REG-001-documents.json"
EPISODES_REL = "registry/SL-REG-002-episodes.json"
ONTOLOGY_REL = "docs/gov/SL-GOV-003-capability-ontology.md"
PARITY_REL = "docs/gov/SL-GOV-010-parity-matrix.md"

SCHEMA_ID = "sl-reg-004/1"
KEYS = ("id", "subsystem", "boundary", "interface_doc", "introduced_in", "unreal_counterpart", "src")
BOUNDARIES = ("core", "module", "plugin", "external", "hosted")
SUBSYSTEMS = ["%02d" % n for n in range(1, 13)]
ID_RE = re.compile(r"^CAP-(\d{2})-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
EP_RE = re.compile(r"^SL-EP-\d{3}$")
ARCH_RE = re.compile(r"^SL-ARCH-\d{3}$")
SRC_SUBSYS = "SL-PLAN-001 §2.2"
SRC_ROW_RE = re.compile(r"^SL-PLAN-001 §2\.7 row (\d+)$")
CLAIM_RE = re.compile(r"\b(?:rivals?|beats?|better than)\b", re.IGNORECASE)
QUOTED_RE = re.compile(r'"[^"\n]*"|“[^”\n]*”')
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def _is_str(value):
    return isinstance(value, str) and value.strip() != ""


def plan_subsystems(plan_text):
    """Section 2.2 as [{n, name, owns, derives}]."""
    lines = cc._section(plan_text, "2.2")
    if lines is None:
        raise ValueError("%s: section 2.2 not found" % PLAN_REL)
    rows = cc._table_rows(lines)
    if any(len(r) != 4 for r in rows):
        raise ValueError("%s: section 2.2 table rows must have 4 cells" % PLAN_REL)
    return [{"n": r[0], "name": r[1], "owns": r[2], "derives": r[3]} for r in rows]


def plan_parity(plan_text):
    """Section 2.7 as [{n, unreal, sliced, volumes, obligation}], n counted from 1 in plan order."""
    lines = cc._section(plan_text, "2.7")
    if lines is None:
        raise ValueError("%s: section 2.7 not found" % PLAN_REL)
    rows = cc._table_rows(lines)
    if any(len(r) != 4 for r in rows):
        raise ValueError("%s: section 2.7 table rows must have 4 cells" % PLAN_REL)
    out = []
    for n, r in enumerate(rows, start=1):
        volumes = ["%02d" % int(v) for v in re.findall(r"\d+", r[2])]
        out.append({"n": n, "unreal": r[0], "sliced": r[1], "volumes": volumes, "obligation": r[3]})
    return out


def shape_errors(reg, label=REGISTER_REL):
    rule = "R-CAP-SHAPE"
    if not isinstance(reg, dict) or set(reg) != {"schema", "capabilities"}:
        return ["%s: %s: top level must be exactly {schema, capabilities}" % (label, rule)]
    errors = []
    if reg["schema"] != SCHEMA_ID:
        errors.append("%s: %s: schema must be %r" % (label, rule, SCHEMA_ID))
    caps = reg["capabilities"]
    if not isinstance(caps, list) or not caps:
        return errors + ["%s: %s: capabilities must be a non-empty list" % (label, rule)]
    seen = set()
    for i, cap in enumerate(caps):
        where = "capabilities[%d]" % i
        if not isinstance(cap, dict):
            errors.append("%s: %s: %s is not an object" % (label, rule, where))
            continue
        missing = [k for k in KEYS if k not in cap]
        extra = [k for k in cap if k not in KEYS]
        if missing or extra:
            errors.append(
                "%s: %s: %s keys: missing %s, unexpected %s" % (label, rule, where, missing or "none", extra or "none")
            )
            continue
        cid = cap["id"]
        where = str(cid)
        m = ID_RE.match(cid) if isinstance(cid, str) else None
        if not m:
            errors.append("%s: %s: %s: id must look like CAP-04-NAME" % (label, rule, where))
        elif m.group(1) != cap["subsystem"]:
            errors.append(
                "%s: %s: %s: id subsystem %s differs from subsystem %r"
                % (label, rule, where, m.group(1), cap["subsystem"])
            )
        if cid in seen:
            errors.append("%s: %s: duplicate id %s" % (label, rule, cid))
        seen.add(cid)
        if cap["boundary"] not in BOUNDARIES:
            errors.append(
                "%s: %s: %s: boundary %r must be one of %s"
                % (label, rule, where, cap["boundary"], "|".join(BOUNDARIES))
            )
        for k in ("subsystem", "interface_doc", "introduced_in", "src"):
            if not _is_str(cap[k]):
                errors.append("%s: %s: %s: %s must be a non-empty string" % (label, rule, where, k))
        if cap["unreal_counterpart"] is not None and not _is_str(cap["unreal_counterpart"]):
            errors.append("%s: %s: %s: unreal_counterpart must be a string or null" % (label, rule, where))
    return errors


def ref_errors(reg, doc_ids, episode_ids, label=REGISTER_REL):
    """R-CAP-SUBSYS (range), R-CAP-DOC and R-CAP-EP for every capability."""
    errors = []
    for cap in reg["capabilities"]:
        cid = cap["id"]
        if cap["subsystem"] not in SUBSYSTEMS:
            errors.append("%s: R-CAP-SUBSYS: %s: subsystem %r is not 01-12" % (label, cid, cap["subsystem"]))
        doc = cap["interface_doc"]
        if not ARCH_RE.match(str(doc)) or doc not in doc_ids:
            errors.append(
                "%s: R-CAP-DOC: %s: interface_doc %r is not an SL-ARCH document registered in SL-REG-001"
                % (label, cid, doc)
            )
        ep = cap["introduced_in"]
        if not EP_RE.match(str(ep)) or ep not in episode_ids:
            errors.append("%s: R-CAP-EP: %s: introduced_in %r is not an episode of SL-REG-002" % (label, cid, ep))
    return errors


def subsystem_errors(reg, plan_subs, doc_text, label=REGISTER_REL, doc_rel=ONTOLOGY_REL):
    rule = "R-CAP-SUBSYS"
    errors = []
    if [r["n"] for r in plan_subs] != SUBSYSTEMS:
        errors.append("%s: %s: master plan section 2.2 does not list subsystems 01..12" % (PLAN_REL, rule))
    by_src = {}
    for cap in reg["capabilities"]:
        if cap["src"] == SRC_SUBSYS:
            by_src.setdefault(cap["subsystem"], []).append(cap["id"])
    for row in plan_subs:
        found = by_src.get(row["n"], [])
        if len(found) != 1:
            errors.append(
                "%s: %s: subsystem %s (%s) needs exactly one capability with src '%s', has %d"
                % (label, rule, row["n"], row["name"], SRC_SUBSYS, len(found))
            )
    block, errs = cc._block_with(doc_text, doc_rel, rule, "subsystems")
    errors += errs
    if block is None:
        return errors
    have = block["subsystems"]
    if not isinstance(have, list) or not all(isinstance(r, dict) for r in have):
        return errors + ["%s: %s: 'subsystems' must be a list of objects" % (doc_rel, rule)]
    if len(have) != len(plan_subs):
        errors.append(
            "%s: %s: %d subsystems, master plan section 2.2 has %d" % (doc_rel, rule, len(have), len(plan_subs))
        )
    ids = {c["id"] for c in reg["capabilities"]}
    for row, got in zip(plan_subs, have):
        for key in ("n", "name", "owns", "derives"):
            if not isinstance(got.get(key), str) or cc.norm(got[key]) != row[key]:
                errors.append(
                    "%s: %s: subsystem %s.%s is %r, plan has %r"
                    % (doc_rel, rule, row["n"], key, got.get(key), row[key])
                )
        if got.get("capability") not in ids:
            errors.append(
                "%s: %s: subsystem %s names capability %r, which is not in the register"
                % (doc_rel, rule, row["n"], got.get("capability"))
            )
    return errors


def parity_errors(reg, plan_rows, doc_text, label=REGISTER_REL, doc_rel=PARITY_REL):
    rule = "R-CAP-PARITY"
    errors = []
    if not plan_rows:
        return ["%s: %s: master plan section 2.7 has no rows" % (PLAN_REL, rule)]
    by_counterpart = {}
    for cap in reg["capabilities"]:
        if cap["unreal_counterpart"] is not None:
            by_counterpart.setdefault(cap["unreal_counterpart"], []).append(cap)
    for row in plan_rows:
        found = by_counterpart.get(row["unreal"], [])
        if len(found) != 1:
            errors.append(
                "%s: %s: section 2.7 row %d (%s) needs exactly one capability, has %d"
                % (label, rule, row["n"], row["unreal"], len(found))
            )
            continue
        want = "SL-PLAN-001 §2.7 row %d" % row["n"]
        if found[0]["src"] != want:
            errors.append("%s: %s: %s: src is %r, expected %r" % (label, rule, found[0]["id"], found[0]["src"], want))
    plan_names = {r["unreal"] for r in plan_rows}
    for name in by_counterpart:
        if name not in plan_names:
            errors.append("%s: %s: unreal_counterpart %r is not a row of master plan section 2.7" % (label, rule, name))
    for cap in reg["capabilities"]:
        m = SRC_ROW_RE.match(cap["src"])
        if m and (cap["unreal_counterpart"] is None or not 1 <= int(m.group(1)) <= len(plan_rows)):
            errors.append(
                "%s: %s: %s: src %r does not point at a section 2.7 row with a counterpart"
                % (label, rule, cap["id"], cap["src"])
            )
    block, errs = cc._block_with(doc_text, doc_rel, rule, "rows")
    errors += errs
    if block is None:
        return errors
    policy = block.get("benchmark")
    if not (isinstance(policy, dict) and policy.get("scenes") == "SL-BENCH" and policy.get("audit") == "Volume 41"):
        errors.append("%s: %s: 'benchmark' policy must name scenes SL-BENCH and audit Volume 41" % (doc_rel, rule))
    rows = block["rows"]
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        return errors + ["%s: %s: 'rows' must be a list of objects" % (doc_rel, rule)]
    if len(rows) != len(plan_rows):
        errors.append("%s: %s: %d rows, master plan section 2.7 has %d" % (doc_rel, rule, len(rows), len(plan_rows)))
    for want, got in zip(plan_rows, rows):
        for key in ("unreal", "sliced", "obligation"):
            if not isinstance(got.get(key), str) or cc.norm(got[key]) != want[key]:
                errors.append(
                    "%s: %s: row %d.%s is %r, plan has %r" % (doc_rel, rule, want["n"], key, got.get(key), want[key])
                )
        if got.get("n") != want["n"] or got.get("volumes") != want["volumes"]:
            errors.append(
                "%s: %s: row %d n/volumes are %r/%r, plan has %r/%r"
                % (doc_rel, rule, want["n"], got.get("n"), got.get("volumes"), want["n"], want["volumes"])
            )
        cap = (by_counterpart.get(want["unreal"]) or [None])[0]
        if cap is None or got.get("capability") != cap["id"]:
            errors.append(
                "%s: %s: row %d capability is %r, register has %r"
                % (doc_rel, rule, want["n"], got.get("capability"), cap and cap["id"])
            )
    return errors


def ontology_errors(doc_text, doc_rel=ONTOLOGY_REL):
    rule = "R-CAP-ONTOLOGY"
    block, errors = cc._block_with(doc_text, doc_rel, rule, "boundaries")
    if block is None:
        return errors
    classes = (
        [b.get("class") for b in block["boundaries"] if isinstance(b, dict)]
        if isinstance(block["boundaries"], list)
        else None
    )
    if classes != list(BOUNDARIES):
        errors.append(
            "%s: %s: 'boundaries' must list %s in order, has %r" % (doc_rel, rule, "|".join(BOUNDARIES), classes)
        )
    ext = block.get("extensibility")
    if not isinstance(ext, list) or not ext or not all(_is_str(x) for x in ext):
        errors.append("%s: %s: 'extensibility' must be a non-empty list of strings" % (doc_rel, rule))
    return errors


def claim_errors(text, label, obligations=(), strip_quotes=True):
    """R-CAP-CLAIM on a document. Verbatim plan obligations are removed first; double-quoted spans outside
    fenced blocks are removed too (a quoted rule, not a claim). Fenced lines keep their JSON quotes."""
    errors = []
    in_fence = False
    for n, line in enumerate(text.splitlines(), start=1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        scan = line
        for ob in obligations:
            scan = scan.replace(ob, "")
        if strip_quotes and not in_fence:
            scan = QUOTED_RE.sub("", scan)
        m = CLAIM_RE.search(scan)
        if m:
            errors.append(
                "%s:%d: R-CAP-CLAIM: %r is a parity claim; record an obligation to measure (Volume 41) instead"
                % (label, n, m.group(0))
            )
    return errors


def _strings(node):
    if isinstance(node, str):
        yield node
    elif isinstance(node, dict):
        for k, v in node.items():
            yield from _strings(k)
            yield from _strings(v)
    elif isinstance(node, list):
        for v in node:
            yield from _strings(v)


def register_claim_errors(reg, label=REGISTER_REL):
    return [
        "%s: R-CAP-CLAIM: %r is a parity claim" % (label, CLAIM_RE.search(s).group(0))
        for s in _strings(reg)
        if CLAIM_RE.search(s)
    ]


def _id_set(path, key):
    data = slib.load_json(path)
    return data, {item.get("id") for item in data[key] if isinstance(item, dict)}


def check_all(root):
    errors = []
    texts = {}
    for rel in (PLAN_REL, ONTOLOGY_REL, PARITY_REL):
        texts[rel], err = cc._read(root, rel)
        if err:
            errors.append("R-CAP-SUBSYS: " + err)
    if errors:
        return errors
    try:
        reg = slib.load_json(os.path.join(root, *REGISTER_REL.split("/")))
    except slib.LoadError as exc:
        return errors + ["%s: R-CAP-SHAPE: %s" % (REGISTER_REL, exc)]
    errors += shape_errors(reg)
    if errors:
        return errors
    try:
        _, doc_ids = _id_set(os.path.join(root, *DOCS_REL.split("/")), "documents")
        _, episode_ids = _id_set(os.path.join(root, *EPISODES_REL.split("/")), "episodes")
        plan_subs = plan_subsystems(texts[PLAN_REL])
        plan_rows = plan_parity(texts[PLAN_REL])
    except (slib.LoadError, KeyError, TypeError, ValueError) as exc:
        return ["R-CAP-SUBSYS: cannot read inputs: %s" % exc]
    errors += ref_errors(reg, doc_ids, episode_ids)
    errors += subsystem_errors(reg, plan_subs, texts[ONTOLOGY_REL])
    errors += parity_errors(reg, plan_rows, texts[PARITY_REL])
    errors += ontology_errors(texts[ONTOLOGY_REL])
    obligations = [r["obligation"] for r in plan_rows if r["obligation"]]
    errors += register_claim_errors(reg)
    for rel in (ONTOLOGY_REL, PARITY_REL):
        errors += claim_errors(texts[rel], rel, obligations)
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_caps.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("capability-check", check_all(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
