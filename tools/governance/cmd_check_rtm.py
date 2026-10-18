"""rtm-check: the SL-GOV-002 requirements catalogue and the SL-REG-003 traceability matrix agree.

Inputs (under --root, default the repository): docs/gov/SL-GOV-002-*.md (its first `sl-data` block holds
{"requirements": [{id, text, src, measure | evidence_type}]}), registry/SL-REG-003-traceability.json
({schema: "sl-reg-003/1", requirements: [row]}), registry/SL-REG-001-documents.json, registry/SL-REG-002-episodes.json
and the justfile. A row is {id, text, src, measure {num, den, threshold} | evidence_type, architecture [doc id],
episodes [SL-EP-nnn], tests ["just <recipe>" | "deferred:SL-EP-nnn"], status}.

Rules (each failure line contains the rule id):
  R-RTM-BLOCK     the catalogue is missing, or has no valid sl-data block of requirements
  R-RTM-SHAPE     the register is missing or a row has a missing, extra or mistyped field
  R-RTM-ID        an id is not REQ-<AREA>-nnn with a known area, or is duplicated
  R-RTM-SRC       a source does not cite the master plan or an ADR
  R-RTM-EVIDENCE  a row has both or neither of measure and evidence_type, or an incomplete measure
  R-RTM-MISSING   a catalogue requirement has no register row
  R-RTM-ORPHAN    a register row has no catalogue requirement
  R-RTM-DRIFT     text, src, measure or evidence_type differ between the catalogue and the register
  R-RTM-ARCH      architecture is empty or names a document absent from SL-REG-001
  R-RTM-EPISODE   episodes is empty or names an episode absent from SL-REG-002
  R-RTM-TEST      tests is empty, or an entry is not an existing `just <recipe>` or a `deferred:` existing episode
  R-RTM-STATUS    status is not one of planned, specified, implemented, verified

Stdlib only (ADR-0004); run with `python3 -I cmd_check_rtm.py [--root DIR]`.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402
import slib  # noqa: E402

DOC_DIR = "docs/gov"
DOC_PREFIX = "SL-GOV-002"
REGISTER_REL = "registry/SL-REG-003-traceability.json"
DOCS_REL = "registry/SL-REG-001-documents.json"
EPISODES_REL = "registry/SL-REG-002-episodes.json"
AREAS = ("DOCTRINE", "FRAG", "EXEC", "GOV", "QUALITY", "RELEASE", "USERS")
STATUSES = ("planned", "specified", "implemented", "verified")
ROW_KEYS = ("id", "text", "src", "architecture", "episodes", "tests", "status")
OPTIONAL_KEYS = ("measure", "evidence_type")
MEASURE_KEYS = ("num", "den", "threshold")
DOC_KEYS = ("id", "text", "src", "measure", "evidence_type")

ID_RE = re.compile(r"^REQ-(%s)-[0-9]{3}$" % "|".join(AREAS))
EP_RE = re.compile(r"^SL-EP-[0-9]{3}$")
DOC_ID_RE = re.compile(r"^SL-[A-Z]+-[0-9]{2,3}(-SPEC)?$")
JUST_RE = re.compile(r"^just ([A-Za-z_][\w-]*)$")
DEFERRED_RE = re.compile(r"^deferred:(SL-EP-[0-9]{3})$")
SRC_RE = re.compile(r"\bmaster plan\b|\bADR-\d{4}\b", re.IGNORECASE)
OPEN_RE = re.compile(r"^\s*```\s*sl-data\s*$")
CLOSE_RE = re.compile(r"^\s*```\s*$")


def sl_data_blocks(text):
    """Raw text of every ```sl-data fenced block."""
    blocks, current = [], None
    for line in text.splitlines():
        if current is None:
            if OPEN_RE.match(line):
                current = []
        elif CLOSE_RE.match(line):
            blocks.append("\n".join(current))
            current = None
        else:
            current.append(line)
    return blocks


def _nonempty_str(value):
    return isinstance(value, str) and bool(value.strip())


def _str_list(value):
    return isinstance(value, list) and all(isinstance(v, str) for v in value)


def catalogue_requirements(text, label):
    """Return (list of catalogue rows or None, errors)."""
    blocks = sl_data_blocks(text)
    if not blocks:
        return None, ["%s:1: R-RTM-BLOCK: no sl-data block" % label]
    try:
        data = json.loads(blocks[0])
    except ValueError as exc:
        return None, ["%s:1: R-RTM-BLOCK: sl-data block is not valid JSON: %s" % (label, exc)]
    rows = data.get("requirements") if isinstance(data, dict) else None
    if not isinstance(rows, list) or not rows:
        return None, ["%s:1: R-RTM-BLOCK: sl-data block needs a non-empty 'requirements' list" % label]
    errors = []
    for i, row in enumerate(rows):
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            errors.append("%s: R-RTM-BLOCK: requirements[%d] is not an object with an id" % (label, i))
    if errors:
        return None, errors
    return rows, []


def _measure_errors(row, where):
    errors = []
    has_m, has_e = "measure" in row, "evidence_type" in row
    if has_m == has_e:
        return ["%s: R-RTM-EVIDENCE: needs exactly one of measure and evidence_type" % where]
    if has_e:
        if not _nonempty_str(row["evidence_type"]):
            errors.append("%s: R-RTM-EVIDENCE: evidence_type must be a non-empty string" % where)
        return errors
    m = row["measure"]
    if not isinstance(m, dict) or set(m) != set(MEASURE_KEYS) or not all(_nonempty_str(m[k]) for k in MEASURE_KEYS):
        errors.append("%s: R-RTM-EVIDENCE: measure must be {num, den, threshold}, each a non-empty string" % where)
    return errors


def row_errors(row, where):
    """Shape, id, source, evidence and status errors of one register row."""
    if not isinstance(row, dict):
        return ["%s: R-RTM-SHAPE: row is not an object" % where]
    errors = []
    for key in ROW_KEYS:
        if key not in row:
            errors.append("%s: R-RTM-SHAPE: missing field '%s'" % (where, key))
    for key in row:
        if key not in ROW_KEYS and key not in OPTIONAL_KEYS:
            errors.append("%s: R-RTM-SHAPE: unknown field '%s'" % (where, key))
    if errors:
        return errors
    if not isinstance(row["id"], str) or not ID_RE.match(row["id"]):
        errors.append(
            "%s: R-RTM-ID: id %r must look like REQ-<AREA>-nnn, AREA in %s" % (where, row["id"], "|".join(AREAS))
        )
    for key in ("text", "src"):
        if not _nonempty_str(row[key]):
            errors.append("%s: R-RTM-SHAPE: %s must be a non-empty string" % (where, key))
    for key in ("architecture", "episodes", "tests"):
        if not _str_list(row[key]):
            errors.append("%s: R-RTM-SHAPE: %s must be a list of strings" % (where, key))
    if _nonempty_str(row["src"]) and not SRC_RE.search(row["src"]):
        errors.append("%s: R-RTM-SRC: src %r must cite the master plan or an ADR" % (where, row["src"]))
    if row["status"] not in STATUSES:
        errors.append("%s: R-RTM-STATUS: status %r must be one of %s" % (where, row["status"], "|".join(STATUSES)))
    errors += _measure_errors(row, where)
    return errors


def link_errors(row, where, doc_ids, episode_ids, recipes):
    """Architecture, episode and test references of one well-shaped row."""
    errors = []
    if not row["architecture"]:
        errors.append("%s: R-RTM-ARCH: architecture is empty" % where)
    for doc_id in row["architecture"]:
        if not DOC_ID_RE.match(doc_id) or doc_id not in doc_ids:
            errors.append("%s: R-RTM-ARCH: %s is not a document in SL-REG-001" % (where, doc_id))
    if not row["episodes"]:
        errors.append("%s: R-RTM-EPISODE: episodes is empty" % where)
    for ep in row["episodes"]:
        if not EP_RE.match(ep) or ep not in episode_ids:
            errors.append("%s: R-RTM-EPISODE: %s is not an episode in SL-REG-002" % (where, ep))
    if not row["tests"]:
        errors.append("%s: R-RTM-TEST: tests is empty" % where)
    for test in row["tests"]:
        m = JUST_RE.match(test)
        d = DEFERRED_RE.match(test)
        if m:
            if m.group(1) not in recipes:
                errors.append("%s: R-RTM-TEST: '%s' names no recipe in the justfile" % (where, test))
        elif d:
            if d.group(1) not in episode_ids:
                errors.append("%s: R-RTM-TEST: '%s' defers to an episode absent from SL-REG-002" % (where, test))
        else:
            errors.append("%s: R-RTM-TEST: '%s' must be 'just <recipe>' or 'deferred:SL-EP-nnn'" % (where, test))
    return errors


def rtm_errors(catalogue, register, doc_ids, episode_ids, recipes, doc_label="SL-GOV-002", reg_label="SL-REG-003"):
    """All errors for parsed inputs. catalogue: list of dicts; register: the SL-REG-003 value."""
    errors = []
    if (
        not isinstance(register, dict)
        or register.get("schema") != "sl-reg-003/1"
        or not isinstance(register.get("requirements"), list)
    ):
        return ["%s: R-RTM-SHAPE: register must be {schema: 'sl-reg-003/1', requirements: [...]}" % reg_label]
    rows = register["requirements"]
    by_id, seen = {}, set()
    for i, row in enumerate(rows):
        where = "%s %s" % (reg_label, row.get("id", "#%d" % i) if isinstance(row, dict) else "#%d" % i)
        errs = row_errors(row, where)
        errors += errs
        rid = row.get("id") if isinstance(row, dict) else None
        if isinstance(rid, str):
            if rid in seen:
                errors.append("%s: R-RTM-ID: duplicate id in the register" % where)
            seen.add(rid)
            by_id[rid] = row
        if not errs:
            errors += link_errors(row, where, doc_ids, episode_ids, recipes)
    cat_ids = set()
    for crow in catalogue:
        rid = crow["id"]
        where = "%s %s" % (doc_label, rid)
        if not ID_RE.match(rid):
            errors.append("%s: R-RTM-ID: id must look like REQ-<AREA>-nnn, AREA in %s" % (where, "|".join(AREAS)))
        if rid in cat_ids:
            errors.append("%s: R-RTM-ID: duplicate id in the catalogue" % where)
        cat_ids.add(rid)
        reg = by_id.get(rid)
        if reg is None:
            errors.append("%s: R-RTM-MISSING: no row in %s" % (where, reg_label))
            continue
        for key in DOC_KEYS:
            if crow.get(key) != reg.get(key):
                errors.append("%s: R-RTM-DRIFT: %s differs between %s and %s" % (where, key, doc_label, reg_label))
    for rid in sorted(by_id):
        if rid not in cat_ids:
            errors.append("%s %s: R-RTM-ORPHAN: no requirement in %s" % (reg_label, rid, doc_label))
    return errors


def _load(root, rel):
    return slib.load_json(os.path.join(root, *rel.split("/")))


def find_catalogue(root):
    path = os.path.join(root, *DOC_DIR.split("/"))
    names = (
        sorted(n for n in os.listdir(path) if n.startswith(DOC_PREFIX) and n.endswith(".md"))
        if os.path.isdir(path)
        else []
    )
    return names[0] if names else None


def check(root):
    name = find_catalogue(root)
    if name is None:
        return ["%s/%s-*.md: R-RTM-BLOCK: the requirements catalogue does not exist" % (DOC_DIR, DOC_PREFIX)]
    label = "%s/%s" % (DOC_DIR, name)
    try:
        with open(os.path.join(root, DOC_DIR, name), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return ["%s:1: R-RTM-BLOCK: unreadable: %s" % (label, exc)]
    catalogue, errors = catalogue_requirements(text, label)
    if errors:
        return errors
    try:
        register = _load(root, REGISTER_REL)
    except slib.LoadError as exc:
        return ["%s: R-RTM-SHAPE: %s" % (REGISTER_REL, exc)]
    try:
        docs = _load(root, DOCS_REL)["documents"]
        eps = _load(root, EPISODES_REL)["episodes"]
        doc_ids = {d["id"] for d in docs}
        episode_ids = {e["id"] for e in eps}
        with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
            recipes = set(slgov.parse_justfile(fh.read()))
    except (slib.LoadError, KeyError, TypeError, OSError) as exc:
        return ["%s: R-RTM-SHAPE: cannot read the reference registers or justfile: %s" % (root, exc)]
    return rtm_errors(catalogue, register, doc_ids, episode_ids, recipes, label, REGISTER_REL)


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_rtm.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("rtm-check", check(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
