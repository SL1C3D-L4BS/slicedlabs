"""registry-docs-check: SL-REG-001 (registry/SL-REG-001-documents.json) is sound and current.

Rules (each failure line contains the rule id):
  R-DOCS-INPUT     the master plan, registry/vol01-documents.json and approvals parse
  R-DOCS-SCHEMA    the committed register, and its derived view, validate against schemas/sl-reg-001
  R-DOCS-DRIFT     the committed register equals what cmd_gen_docs.py would produce
  R-DOCS-COUNT     104 documents: 62 named plus 42 SL-VOL-nn-SPEC
  R-DOCS-FRONT     a registered document file has valid front matter (id, status) per docs/templates/DOC.md
  R-DOCS-APPROVAL  Approved comes only from registry/SL-REG-001.approvals.json with a matching sha256
  R-DOCS-UNREGISTERED  every document file under docs/gov, docs/plan, docs/vol and registry/SL-REG-nnn-*.json
                   is a registered path

Status, version and sha256 are derived at check time (cmd_gen_docs.derive_view), never stored.
Stdlib only (ADR-0004); run with `python3 -I cmd_check_docs_registry.py [--root DIR]`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_gen_docs  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

SCAN = (
    ("docs/gov", re.compile(r"^SL-GOV-\d{3}.*\.md$")),
    ("docs/plan", re.compile(r"^SL-PLAN-\d{3}.*\.md$")),
    ("docs/vol", re.compile(r"^SL-VOL-\d{2}-SPEC\.md$")),
    ("registry", re.compile(r"^SL-REG-\d{3}-[A-Za-z0-9-]+\.json$")),
)
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


def _schema_errors(label, instance, schema):
    return ["%s: R-DOCS-SCHEMA: %s: %s" % (label, ptr or "/", msg) for ptr, msg in slib.validate(instance, schema)]


def docs_errors(root):
    errors = []
    reg_path = os.path.join(root, *cmd_gen_docs.REGISTER_REL.split("/"))
    try:
        committed = slib.load_json(reg_path)
    except slib.LoadError as exc:
        return ["%s: R-DOCS-SCHEMA: %s" % (cmd_gen_docs.REGISTER_REL, exc)]
    schema = slib.load_schema("sl-reg-001", os.path.join(root, "schemas"))
    errors += _schema_errors(cmd_gen_docs.REGISTER_REL, committed, schema)
    try:
        expected = cmd_gen_docs.build(root)
    except (cmd_gen_docs.DocsError, OSError) as exc:
        return errors + ["%s: R-DOCS-INPUT: %s" % (cmd_gen_docs.PLAN_REL, exc)]
    try:
        with open(reg_path, encoding="utf-8") as fh:
            on_disk = fh.read()
    except OSError:
        on_disk = None
    if on_disk != cmd_gen_docs.render(expected):
        errors.append(
            "%s:1: R-DOCS-DRIFT: differs from the generator output; run python3 -I tools/governance/cmd_gen_docs.py"
            % cmd_gen_docs.REGISTER_REL
        )
    docs = expected["documents"]
    named = [d for d in docs if d["kind"] != "VOL"]
    if len(docs) != 104 or len(named) != 62:
        errors.append("%s: R-DOCS-COUNT: %d documents (%d named), expected 104 (62 named)" % (cmd_gen_docs.REGISTER_REL, len(docs), len(named)))
    approvals, approvals_error = cmd_gen_docs.load_approvals(root)
    if approvals_error:
        errors.append("%s: R-DOCS-INPUT: %s" % (cmd_gen_docs.APPROVALS_REL, approvals_error))
    view, problems = cmd_gen_docs.derive_view(root, expected, approvals)
    errors += problems
    errors += _schema_errors("derived view", {"schema": cmd_gen_docs.SCHEMA_ID, "documents": view}, schema)
    by_id = {d["id"]: d for d in view}
    for doc_id in sorted(approvals):
        entry, where = approvals[doc_id], "%s[%s]" % (cmd_gen_docs.APPROVALS_REL, doc_id)
        if doc_id not in by_id:
            errors.append("%s: R-DOCS-APPROVAL: not a registered document" % where)
            continue
        if not isinstance(entry, dict) or not isinstance(entry.get("sha256"), str) or not SHA_RE.match(entry["sha256"]):
            errors.append("%s: R-DOCS-APPROVAL: entry must be an object with a 64-hex sha256" % where)
            continue
        actual = by_id[doc_id]["sha256"]
        if actual is None:
            errors.append("%s: R-DOCS-APPROVAL: the document file %s does not exist" % (where, by_id[doc_id]["path"]))
        elif entry["sha256"] != actual:
            errors.append("%s: R-DOCS-APPROVAL: sha256 does not match %s (%s)" % (where, by_id[doc_id]["path"], actual))
    for doc in view:
        if doc["status"] == "InReview" and doc["id"] not in approvals and _claims_approved(root, doc):
            errors.append(
                "%s: R-DOCS-APPROVAL: front matter says Approved but %s has no entry in %s"
                % (doc["path"], doc["id"], cmd_gen_docs.APPROVALS_REL)
            )
        elif doc["status"] == "Approved" and not _claims_approved(root, doc) and doc["path"].endswith(".md") and doc["id"] not in cmd_gen_docs.NO_FRONT_MATTER:
            errors.append("%s: R-DOCS-APPROVAL: approved in %s but front matter status is not Approved" % (doc["path"], cmd_gen_docs.APPROVALS_REL))
    registered = {d["path"] for d in docs if d["path"]}
    for rel_dir, pattern in SCAN:
        directory = os.path.join(root, *rel_dir.split("/"))
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            rel = "%s/%s" % (rel_dir, name)
            if pattern.match(name) and os.path.isfile(os.path.join(directory, name)) and rel not in registered:
                errors.append("%s: R-DOCS-UNREGISTERED: not a registered path in %s" % (rel, cmd_gen_docs.REGISTER_REL))
    return errors


def _claims_approved(root, doc):
    """True when the document's own front matter status is Approved."""
    try:
        with open(os.path.join(root, *doc["path"].split("/")), encoding="utf-8") as fh:
            return slgov.parse_front_matter(fh.read()).get("status") == "Approved"
    except (OSError, UnicodeDecodeError, slgov.FrontMatterError):
        return False


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_docs_registry.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("registry-docs-check", docs_errors(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
