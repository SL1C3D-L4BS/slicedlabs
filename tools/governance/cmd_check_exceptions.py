"""exception-check: registry/SL-REG-006-exceptions.json (the F-budget exception register) is sound.

Rules (each failure line starts with the rule id):
  R-EXC-INPUT      the register parses as JSON without duplicate keys
  R-EXC-SCHEMA     the register validates against schemas/sl-reg-006.schema.json
  R-EXC-CLASSES    `classes` names are exactly the six allowed classes of master plan section 3.1, once each
                   and (when SL-GOV-006 is present) equal to its `exemptions` list
  R-EXC-DUP-ID     exception ids and class ids are unique
  R-EXC-CLASS      an exception's `class` is the id of a class in `classes`
  R-EXC-INVARIANT  an exception's `subject` names one invariant F1..F8 (the schema has no invariant field)
  R-EXC-REASON     an exception has a non-blank `reason`
  R-EXC-OWNER      an exception has a non-blank `approver` (the owner field of the work order)
  R-EXC-EXPIRES    `expires` is a real calendar date (the schema allows null; this gate does not)
  R-EXC-REVIEW     `approved` is a real calendar date and not after `expires` (every exemption is reviewed
                   every release, master plan section 3.1; the schema has no review field)

Stdlib only (ADR-0004); run with `python3 -I cmd_check_exceptions.py [--root DIR]`.
"""

import datetime
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

REGISTER_REL = "registry/SL-REG-006-exceptions.json"
FRAGMENTATION_REL = "docs/gov/SL-GOV-006-fragmentation-budget.md"
# master plan section 3.1, in the order SL-GOV-006 lists them
CLASSES = (
    "console certification",
    "platform signing",
    "specialist simulation solvers",
    "legal or regulatory validation",
    "interchange import/export",
    "AI model training",
)
INVARIANT_RE = re.compile(r"(?<![A-Za-z0-9])F[1-8](?![A-Za-z0-9])")
ANY_F_RE = re.compile(r"(?<![A-Za-z0-9])F\d+(?![A-Za-z0-9])")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def parse_date(value):
    """A datetime.date for a real YYYY-MM-DD string, else None."""
    if not isinstance(value, str) or not DATE_RE.match(value):
        return None
    try:
        return datetime.date.fromisoformat(value)
    except ValueError:
        return None


def blank(value):
    return not isinstance(value, str) or not value.strip()


def class_errors(register, doc_exemptions=None):
    errors = []
    classes = register.get("classes")
    if not isinstance(classes, list):
        return errors
    names = [c.get("name") if isinstance(c, dict) else None for c in classes]
    if names != list(CLASSES):
        errors.append(
            "%s: R-EXC-CLASSES: classes are %r, master plan section 3.1 allows exactly %r" % (REGISTER_REL, names, list(CLASSES))
        )
    if doc_exemptions is not None and list(doc_exemptions) != list(CLASSES):
        errors.append(
            "%s: R-EXC-CLASSES: SL-GOV-006 exemptions %r differ from the six classes %r"
            % (FRAGMENTATION_REL, list(doc_exemptions), list(CLASSES))
        )
    ids = [c.get("id") for c in classes if isinstance(c, dict)]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        errors.append("%s: R-EXC-DUP-ID: class id %s appears more than once" % (REGISTER_REL, dup))
    return errors


def exception_errors(register):
    errors = []
    classes = register.get("classes")
    exceptions = register.get("exceptions")
    if not isinstance(exceptions, list):
        return errors
    class_ids = {c.get("id") for c in classes if isinstance(c, dict)} if isinstance(classes, list) else set()
    seen = set()
    for i, exc in enumerate(exceptions):
        if not isinstance(exc, dict):
            continue
        label = "%s: exceptions[%d] %s" % (REGISTER_REL, i, exc.get("id", "?"))
        if exc.get("id") in seen:
            errors.append("%s: R-EXC-DUP-ID: id appears more than once" % label)
        seen.add(exc.get("id"))
        if exc.get("class") not in class_ids:
            errors.append("%s: R-EXC-CLASS: class %r is not one of %s" % (label, exc.get("class"), sorted(class_ids)))
        subject = exc.get("subject")
        found = INVARIANT_RE.findall(subject) if isinstance(subject, str) else []
        if len(set(found)) != 1 or len(set(ANY_F_RE.findall(subject))) != 1:
            errors.append("%s: R-EXC-INVARIANT: subject %r must name exactly one invariant F1..F8" % (label, subject))
        if blank(exc.get("reason")):
            errors.append("%s: R-EXC-REASON: reason must not be blank" % label)
        if blank(exc.get("approver")):
            errors.append("%s: R-EXC-OWNER: approver (owner) must not be blank" % label)
        expires = parse_date(exc.get("expires"))
        if expires is None:
            errors.append("%s: R-EXC-EXPIRES: expires %r must be a real date YYYY-MM-DD" % (label, exc.get("expires")))
        approved = parse_date(exc.get("approved"))
        if approved is None:
            errors.append("%s: R-EXC-REVIEW: approved %r must be a real date YYYY-MM-DD" % (label, exc.get("approved")))
        elif expires is not None and approved > expires:
            errors.append("%s: R-EXC-REVIEW: approved %s is after expires %s" % (label, approved, expires))
    return errors


def register_errors(register, schema, doc_exemptions=None):
    """All errors for a loaded register against the schema (and the SL-GOV-006 list when given)."""
    errors = ["%s: R-EXC-SCHEMA: %s: %s" % (REGISTER_REL, ptr or "/", msg) for ptr, msg in slib.validate(register, schema)]
    if isinstance(register, dict):
        errors += class_errors(register, doc_exemptions)
        errors += exception_errors(register)
    return errors


def doc_exemptions(root):
    """The `exemptions` list of SL-GOV-006, or None when the document or the key is absent."""
    try:
        with open(os.path.join(root, *FRAGMENTATION_REL.split("/")), encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return None
    blocks, _ = cmd_constitution_check.sl_data_blocks(text)
    for block in blocks:
        if isinstance(block, dict) and "exemptions" in block:
            return block["exemptions"]
    return None


def run(root):
    try:
        register = slib.load_json(os.path.join(root, *REGISTER_REL.split("/")))
        schema = slib.load_schema("sl-reg-006", os.path.join(root, "schemas"))
    except slib.LoadError as exc:
        return ["%s: R-EXC-INPUT: %s" % (REGISTER_REL, exc)]
    return register_errors(register, schema, doc_exemptions(root))


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_exceptions.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("exception-check", run(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
