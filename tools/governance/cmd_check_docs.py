"""doc-check: controlled documents in docs/gov, docs/arch, docs/plan and docs/vol follow docs/templates/DOC.md.

Rules (each failure line starts with the rule id):
  R-DOC-FRONT    front matter parses and has every key with a valid value
  R-DOC-NAME     the file is <id>.md or <id>-<slug>.md and the id's kind matches `kind`
  R-DOC-SECTION  the level-2 headings are exactly the seven template sections, in order
  R-DOC-CITE     every line with MUST, SHOULD or MAY cites a section sign, "section", "master plan" or ADR-NNNN
  R-DOC-LENGTH   at most 150 lines unless front matter sets max_lines

SL-PLAN-001 is a verbatim copy of the master plan and is exempt. Stdlib only (ADR-0004); run with
`python3 -I cmd_check_docs.py [--root DIR]`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

DOC_DIRS = ("docs/gov", "docs/arch", "docs/plan", "docs/vol")
EXEMPT_PREFIX = "SL-PLAN-001"
KINDS = ("GOV", "ARCH", "PLAN", "OPS", "REG", "VOL")
STATUSES = ("Draft", "InReview", "Approved", "Superseded")
LIST_KEYS = ("episodes", "depends_on", "supersedes", "decisions")
REQUIRED_KEYS = ("id", "kind", "title", "status", "version", "owner") + LIST_KEYS
SECTIONS = (
    "Purpose",
    "Status and decisions",
    "Normative text",
    "Measures and enforcement points",
    "Traceability",
    "Open issues",
    "Change log",
)
DEFAULT_MAX_LINES = 150

ID_RE = re.compile(r"^SL-(GOV|ARCH|PLAN|OPS|REG|VOL)-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
NORMATIVE_RE = re.compile(r"\b(?:MUST|SHOULD|MAY)\b")
CITATION_RE = re.compile(r"§|\bsections?\b|\bmaster plan\b|\bADR-\d{4}\b", re.IGNORECASE)
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def body_lines(text):
    """Yield (line number, line, in_fence) for the document body (after the front matter)."""
    lines = text.splitlines()
    start = 0
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                start = i + 1
                break
    fence = None
    for n in range(start, len(lines)):
        line = lines[n]
        m = FENCE_RE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)
                yield n + 1, line, True
                continue
            if m.group(1) == fence:
                fence = None
                yield n + 1, line, True
                continue
        yield n + 1, line, fence is not None


def citation_errors(text, label, citation_re=CITATION_RE, rule="R-DOC-CITE"):
    """Errors for every non-fenced line with MUST, SHOULD or MAY and no citation."""
    errors = []
    in_comment = False
    for n, line, in_fence in body_lines(text):
        if in_fence:
            continue
        if "<!--" in line:
            in_comment = "-->" not in line
            continue
        if in_comment:
            in_comment = "-->" not in line
            continue
        if NORMATIVE_RE.search(line) and not citation_re.search(line):
            errors.append(
                "%s:%d: %s: normative line (MUST/SHOULD/MAY) has no citation; cite a master plan "
                "section or ADR-NNNN, or move it to Open issues" % (label, n, rule)
            )
    return errors


def section_errors(text, label):
    found = [(n, line[3:].strip()) for n, line, in_fence in body_lines(text) if not in_fence and line.startswith("## ")]
    names = [name for _, name in found]
    if names == list(SECTIONS):
        return []
    missing = [s for s in SECTIONS if s not in names]
    detail = []
    if missing:
        detail.append("missing: " + ", ".join(missing))
    extra = [n for n in names if n not in SECTIONS]
    if extra:
        detail.append("unexpected: " + ", ".join(extra))
    if not missing and not extra:
        detail.append("out of order")
    return ["%s:1: R-DOC-SECTION: sections must be %s (%s)" % (label, " / ".join(SECTIONS), "; ".join(detail))]


def front_errors(meta, label, filename):
    errors = []
    for key in REQUIRED_KEYS:
        if key not in meta:
            errors.append("%s:1: R-DOC-FRONT: front matter lacks '%s'" % (label, key))
    if errors:
        return errors
    doc_id = meta["id"]
    if not isinstance(doc_id, str) or not ID_RE.match(doc_id):
        errors.append("%s:1: R-DOC-FRONT: id %r must look like SL-GOV-001 or SL-VOL-01-SPEC" % (label, doc_id))
    if meta["kind"] not in KINDS:
        errors.append("%s:1: R-DOC-FRONT: kind %r must be one of %s" % (label, meta["kind"], "|".join(KINDS)))
    if meta["status"] not in STATUSES:
        errors.append("%s:1: R-DOC-FRONT: status %r must be one of %s" % (label, meta["status"], "|".join(STATUSES)))
    if not isinstance(meta["version"], str) or not VERSION_RE.match(meta["version"]):
        errors.append("%s:1: R-DOC-FRONT: version %r must look like 0.1.0" % (label, meta["version"]))
    for key in ("title", "owner"):
        if not isinstance(meta[key], str) or not meta[key].strip():
            errors.append("%s:1: R-DOC-FRONT: %s must not be empty" % (label, key))
    for key in LIST_KEYS:
        if not isinstance(meta[key], list):
            errors.append("%s:1: R-DOC-FRONT: %s must be a list such as [a, b]" % (label, key))
    if "max_lines" in meta and not re.match(r"^[1-9]\d*$", str(meta["max_lines"])):
        errors.append("%s:1: R-DOC-FRONT: max_lines must be a positive integer" % label)
    if errors:
        return errors
    stem = filename[:-3]
    if stem != doc_id and not stem.startswith(doc_id + "-"):
        errors.append("%s:1: R-DOC-NAME: file name must be %s.md or %s-<slug>.md" % (label, doc_id, doc_id))
    if doc_id.split("-")[1] != meta["kind"]:
        errors.append("%s:1: R-DOC-NAME: id %s does not match kind %s" % (label, doc_id, meta["kind"]))
    return errors


def check_document(text, label, filename):
    """All errors for one document's text."""
    try:
        meta = slgov.parse_front_matter(text)
    except slgov.FrontMatterError as exc:
        return ["%s:1: R-DOC-FRONT: %s" % (label, exc)]
    errors = front_errors(meta, label, filename)
    errors += section_errors(text, label)
    errors += citation_errors(text, label)
    limit = DEFAULT_MAX_LINES
    if str(meta.get("max_lines", "")).isdigit():
        limit = int(meta["max_lines"])
    count = len(text.splitlines())
    if count > limit:
        errors.append(
            "%s:1: R-DOC-LENGTH: %d lines exceed the limit of %d (set max_lines to change it)" % (label, count, limit)
        )
    return errors


def doc_errors(root):
    errors = []
    for rel_dir in DOC_DIRS:
        path = os.path.join(root, *rel_dir.split("/"))
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not (name.startswith("SL-") and name.endswith(".md")) or name.startswith(EXEMPT_PREFIX):
                continue
            label = "%s/%s" % (rel_dir, name)
            try:
                with open(os.path.join(path, name), encoding="utf-8") as fh:
                    text = fh.read()
            except (OSError, UnicodeDecodeError) as exc:
                errors.append("%s:1: R-DOC-FRONT: unreadable: %s" % (label, exc))
                continue
            errors += check_document(text, label, name)
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_docs.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("doc-check", doc_errors(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
