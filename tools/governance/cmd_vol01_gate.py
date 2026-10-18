"""vol01-gate: the Volume 01 closing gate (WO-022).

Rules (each failure line contains the rule id; pending contracts are listed and counted, never a failure):
  R-V1-SHAPE     registry/vol01-documents.json, SL-REG-001 and the justfile parse
  R-V1-IDS       the ids of vol01-documents.json equal the fixed set of 20 documents, each listed once
  R-V1-FILE      each of the 20 has a file at its SL-REG-001 path: Markdown with valid front matter (matching id,
                 status Draft|InReview|Approved|Superseded), JSON registers that load, SL-PLAN-001 whose sha256
                 equals docs/plan/SL-PLAN-001-master-plan.sha256
  R-V1-OWNER     each document has exactly one authoring_wo (WO-NNN, agreeing with SL-REG-001) and
                 work-orders/WO-NNN.md exists (WO-000 is the committed genesis and is exempt)
  R-V1-EPISODES  SL-EP-001..020 each appear exactly once with one primary document from the 20 and one WO
  R-V1-CONTRACT  every sl-data contract in docs/gov with a verified_by / enforced_by value (or an `enforcement`
                 value when it has neither) is `pending` or names an existing just recipe

Stdlib only (ADR-0004); run with `python3 -I cmd_vol01_gate.py [--root DIR]`.
"""

import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check  # noqa: E402
import slgov  # noqa: E402

REL = "registry/vol01-documents.json"
REG_REL = "registry/SL-REG-001-documents.json"
PLAN_SHA_REL = "docs/plan/SL-PLAN-001-master-plan.sha256"
SCHEMA = "sl-vol01-documents/1"
EXPECTED_IDS = (
    ["SL-GOV-%03d" % n for n in range(1, 11)]
    + ["SL-PLAN-001", "SL-PLAN-002", "SL-PLAN-004", "SL-PLAN-005"]
    + ["SL-REG-001", "SL-REG-002", "SL-REG-003", "SL-REG-004", "SL-REG-006"]
    + ["SL-VOL-01-SPEC"]
)
EXPECTED_EPISODES = ["SL-EP-%03d" % n for n in range(1, 21)]
WO_RE = re.compile(r"^WO-\d{3}$")
GENESIS_WO = "WO-000"
STATUSES = ("Draft", "InReview", "Approved", "Superseded")
VERIFIER_KEYS = ("verified_by", "enforced_by")
_JUST = re.compile(r"^just\s+([A-Za-z_][\w-]*)\b")
_NAME = re.compile(r"^[A-Za-z_][\w-]*$")


def _load_json(root, rel):
    try:
        with open(os.path.join(root, *rel.split("/")), encoding="utf-8") as fh:
            return json.load(fh), None
    except (OSError, ValueError) as exc:
        return None, "%s: R-V1-SHAPE: %s" % (rel, exc)


def _read(root, rel):
    with open(os.path.join(root, *rel.split("/")), "rb") as fh:
        return fh.read()


def _wo_error(root, wo, where, rule):
    if not isinstance(wo, str) or not WO_RE.match(wo):
        return "%s: %s: %s must be exactly one work order id WO-NNN, found %r" % (REL, rule, where, wo)
    if wo != GENESIS_WO and not os.path.isfile(os.path.join(root, "work-orders", wo + ".md")):
        return "%s: %s: %s names %s but work-orders/%s.md does not exist" % (REL, rule, where, wo, wo)
    return None


def _file_error(root, doc_id, path):
    """Return an error string when the document file at `path` is missing or invalid, else None."""
    if not isinstance(path, str) or not path:
        return "%s: R-V1-FILE: %s has no path in %s" % (REG_REL, doc_id, REG_REL)
    full = os.path.join(root, *path.split("/"))
    if not os.path.isfile(full):
        return "%s: R-V1-FILE: %s file is missing" % (path, doc_id)
    if doc_id == "SL-PLAN-001":
        try:
            want = _read(root, PLAN_SHA_REL).decode("ascii").split()[0].lower()
        except (OSError, UnicodeDecodeError, IndexError) as exc:
            return "%s: R-V1-FILE: cannot read the pinned hash: %s" % (PLAN_SHA_REL, exc)
        got = hashlib.sha256(_read(root, path)).hexdigest()
        if got != want:
            return "%s: R-V1-FILE: sha256 %s does not equal %s (%s)" % (path, got, PLAN_SHA_REL, want)
        return None
    if path.endswith(".json"):
        try:
            with open(full, encoding="utf-8") as fh:
                json.load(fh)
        except (OSError, ValueError) as exc:
            return "%s: R-V1-FILE: %s does not load: %s" % (path, doc_id, exc)
        return None
    try:
        with open(full, encoding="utf-8") as fh:
            meta = slgov.parse_front_matter(fh.read())
    except (OSError, UnicodeDecodeError, slgov.FrontMatterError) as exc:
        return "%s:1: R-V1-FILE: %s front matter: %s" % (path, doc_id, exc)
    if meta.get("id") != doc_id:
        return "%s:1: R-V1-FILE: front matter id %r is not %s" % (path, meta.get("id"), doc_id)
    if meta.get("status") not in STATUSES:
        return "%s:1: R-V1-FILE: status %r must be one of %s" % (path, meta.get("status"), "|".join(STATUSES))
    return None


def _walk(node, where):
    if isinstance(node, dict):
        if any(k in node for k in VERIFIER_KEYS + ("enforcement",)):
            yield where, node
        for k, v in node.items():
            yield from _walk(v, "%s.%s" % (where, k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk(v, "%s[%d]" % (where, i))


def _verifier(node):
    """The (key, value) that must name a recipe or pending: verified_by, enforced_by, else enforcement."""
    for key in VERIFIER_KEYS + ("enforcement",):
        if key in node:
            return key, node[key]
    return None, None


def contract_scan(root, recipes):
    """Return (errors, total, pending) for the sl-data contracts of docs/gov.

    `pending` lists (label, reason): the value is `pending`, or names a recipe that is still a stub.
    """
    errors, pending, total = [], [], 0
    stubs = set()
    for name, recipe in recipes.items():
        if slgov.stub_target(recipe) is not None:
            stubs.add(name)
    try:
        manifest = slgov.load_recipes_json(os.path.join(root, "tools", "governance", "recipes.json"))
    except (OSError, ValueError, KeyError):
        manifest = {}
    stubs |= {n for n, m in manifest.items() if m.get("status") == "stub" and n in recipes}
    gov = os.path.join(root, "docs", "gov")
    names = sorted(os.listdir(gov)) if os.path.isdir(gov) else []
    for fname in names:
        if not (fname.startswith("SL-") and fname.endswith(".md")):
            continue
        rel = "docs/gov/" + fname
        try:
            with open(os.path.join(gov, fname), encoding="utf-8") as fh:
                blocks, bad = cmd_constitution_check.sl_data_blocks(fh.read())
        except (OSError, UnicodeDecodeError) as exc:
            errors.append("%s: R-V1-CONTRACT: unreadable: %s" % (rel, exc))
            continue
        errors += ["%s: R-V1-CONTRACT: %s" % (rel, e) for e in bad]
        for b, block in enumerate(blocks):
            for where, node in _walk(block, "block%d" % b):
                key, value = _verifier(node)
                label = "%s %s" % (rel, node.get("id") or node.get("area") or node.get("item") or where)
                total += 1
                texts = value if isinstance(value, list) else [value]
                if not texts or not all(isinstance(t, str) and t.strip() for t in texts):
                    errors.append("%s: R-V1-CONTRACT: %s must be a recipe name or `pending`, found %r" % (label, key, value))
                    continue
                for text in texts:
                    s = text.strip()
                    if s == "pending":
                        pending.append((label, "pending"))
                        continue
                    m = _JUST.match(s)
                    name = m.group(1) if m else (s if _NAME.match(s) else None)
                    if name is None:
                        errors.append("%s: R-V1-CONTRACT: %s %r is neither `pending` nor `just <recipe>`" % (label, key, s))
                    elif name not in recipes:
                        errors.append("%s: R-V1-CONTRACT: %s names recipe %s, which is not in the justfile" % (label, key, name))
                    elif name in stubs:
                        pending.append((label, "recipe %s is still a stub" % name))
    return errors, total, pending


def gate(root):
    """Return (errors, report) where report holds the data for the summary."""
    errors = []
    data, err = _load_json(root, REL)
    if err:
        return [err], None
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        return ["%s: R-V1-SHAPE: schema must be %r" % (REL, SCHEMA)], None
    docs, eps = data.get("documents"), data.get("episodes")
    if not isinstance(docs, list) or not isinstance(eps, list):
        return ["%s: R-V1-SHAPE: 'documents' and 'episodes' must be lists" % REL], None
    reg, err = _load_json(root, REG_REL)
    paths, reg_wo = {}, {}
    if err:
        errors.append(err)
    elif isinstance(reg, dict) and isinstance(reg.get("documents"), list):
        for d in reg["documents"]:
            if isinstance(d, dict) and isinstance(d.get("id"), str):
                paths[d["id"]] = d.get("path")
                reg_wo[d["id"]] = d.get("authoring_wo")
    else:
        errors.append("%s: R-V1-SHAPE: needs a 'documents' list" % REG_REL)
    try:
        with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
            recipes = slgov.parse_justfile(fh.read())
    except OSError as exc:
        errors.append("justfile: R-V1-SHAPE: %s" % exc)
        recipes = {}

    # (1) the fixed set of 20
    ids, owner = [], {}
    for i, entry in enumerate(docs):
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            errors.append("%s: R-V1-SHAPE: documents[%d] needs an 'id' string" % (REL, i))
            continue
        ids.append(entry["id"])
        owner[entry["id"]] = entry.get("authoring_wo")
    for doc_id in sorted(set(ids)):
        if ids.count(doc_id) > 1:
            errors.append("%s: R-V1-IDS: %s is listed %d times" % (REL, doc_id, ids.count(doc_id)))
    for doc_id in EXPECTED_IDS:
        if doc_id not in ids:
            errors.append("%s: R-V1-IDS: %s is missing from the 20" % (REL, doc_id))
    for doc_id in sorted(set(ids) - set(EXPECTED_IDS)):
        errors.append("%s: R-V1-IDS: %s is not one of the 20 Volume 01 documents" % (REL, doc_id))

    # (2) files and (3) authoring work orders
    rows = []
    for doc_id in EXPECTED_IDS:
        path = paths.get(doc_id)
        ferr = _file_error(root, doc_id, path)
        if ferr:
            errors.append(ferr)
        wo = owner.get(doc_id)
        werr = None if doc_id not in owner else _wo_error(root, wo, "%s authoring_wo" % doc_id, "R-V1-OWNER")
        if werr:
            errors.append(werr)
        elif doc_id in owner and reg_wo.get(doc_id) != wo:
            errors.append("%s: R-V1-OWNER: %s authoring_wo %r differs from %s (%r)" % (REL, doc_id, wo, REG_REL, reg_wo.get(doc_id)))
        rows.append((doc_id, path or "-", "ok" if not ferr else "FAIL", wo if isinstance(wo, str) else "-", "ok" if doc_id in owner and not werr else "FAIL"))

    # (4) episodes
    seen, primary = [], {}
    for i, entry in enumerate(eps):
        if not isinstance(entry, dict) or not isinstance(entry.get("episode"), str):
            errors.append("%s: R-V1-SHAPE: episodes[%d] needs an 'episode' string" % (REL, i))
            continue
        ep = entry["episode"]
        seen.append(ep)
        primary[ep] = (entry.get("document"), entry.get("wo"))
        if entry.get("document") not in EXPECTED_IDS:
            errors.append("%s: R-V1-EPISODES: %s primary document %r is not one of the 20" % (REL, ep, entry.get("document")))
        werr = _wo_error(root, entry.get("wo"), "%s wo" % ep, "R-V1-EPISODES")
        if werr:
            errors.append(werr)
    for ep in sorted(set(seen)):
        if seen.count(ep) > 1:
            errors.append("%s: R-V1-EPISODES: %s appears %d times (two primary documents)" % (REL, ep, seen.count(ep)))
    for ep in EXPECTED_EPISODES:
        if ep not in seen:
            errors.append("%s: R-V1-EPISODES: %s has no primary document" % (REL, ep))
    for ep in sorted(set(seen) - set(EXPECTED_EPISODES)):
        errors.append("%s: R-V1-EPISODES: %s is outside 001-020" % (REL, ep))

    # (5) contracts
    cerrors, total, pending = contract_scan(root, recipes)
    errors += cerrors
    return errors, {"rows": rows, "episodes": primary, "contracts": total, "pending": pending}


def render(rep, errors):
    lines = ["%-15s %-52s %-5s %-7s %-5s" % ("document", "path", "file", "wo", "owner")]
    for doc_id, path, fok, wo, ook in rep["rows"]:
        lines.append("%-15s %-52s %-5s %-7s %-5s" % (doc_id, path, fok, wo, ook))
    eps = rep["episodes"]
    lines.append("episodes: %d/20 with a primary document and work order" % sum(1 for e in EXPECTED_EPISODES if e in eps))
    for label, reason in rep["pending"]:
        lines.append("pending: %s: %s" % (label, reason))
    lines.append(
        "contracts: %d total, %d pending (informational, never fails); %d failure(s)"
        % (rep["contracts"], len(rep["pending"]), len(errors))
    )
    return lines


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_vol01_gate.py [--root DIR]", file=sys.stderr)
        return 2
    errors, rep = gate(root)
    if rep is not None:
        for line in render(rep, errors):
            print(line)
    return slgov.report("vol01-gate", errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
