"""adr-check: docs/adr/ADR-NNNN-<slug>.md follow docs/templates/ADR.md, and SL-GOV-004 is current.

Rules (each failure line starts with the rule id):
  R-ADR-FRONT      front matter parses and has id, title, status, date, decision_refs, supersedes
  R-ADR-NAME       the file name is ADR-NNNN-<kebab-slug>.md and NNNN matches the id
  R-ADR-SECTION    the level-2 headings are Context, Decision, Consequences, Alternatives, in order
  R-ADR-CITE       every line with MUST, SHOULD or MAY cites a master plan section, a machine fact
                   or another ADR
  R-ADR-LENGTH     at most 80 lines
  R-ADR-SUPERSEDE  every `supersedes` entry is an existing ADR (not itself) marked Superseded
  R-ADR-0001       ADR-0001 exists
  R-ADR-LOG-STALE  docs/gov/SL-GOV-004-adr-log.md equals the generator output

Stdlib only (ADR-0004); run with `python3 -I cmd_check_adr.py [--root DIR]`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_docs  # noqa: E402
import cmd_gen_adr_log  # noqa: E402
import slgov  # noqa: E402

STATUSES = ("Proposed", "Accepted", "Superseded")
SECTIONS = ("Context", "Decision", "Consequences", "Alternatives")
REQUIRED_KEYS = ("id", "title", "status", "date", "decision_refs", "supersedes")
MAX_LINES = 80
FILE_RE = re.compile(r"^ADR-(\d{4})-([a-z0-9]+(?:-[a-z0-9]+)*)\.md$")
ID_RE = re.compile(r"^ADR-\d{4}$")
REF_RE = re.compile(r"^(?:D-\d+|ADR-\d{4})$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
CITATION_RE = re.compile(
    r"§|\bsections?\b|\bmaster plan\b|\bmachine facts?\b|\bplan of record\b|\bADR-\d{4}\b", re.IGNORECASE
)


def sections_error(text, label):
    names = [
        line[3:].strip()
        for _, line, in_fence in cmd_check_docs.body_lines(text)
        if not in_fence and line.startswith("## ")
    ]
    if names == list(SECTIONS):
        return []
    return [
        "%s:1: R-ADR-SECTION: sections must be %s, in order; found %s"
        % (label, " / ".join(SECTIONS), ", ".join(names) or "none")
    ]


def check_one(name, text):
    """Per-file errors plus (metadata or None)."""
    label = "docs/adr/%s" % name
    m = FILE_RE.match(name)
    errors = []
    if not m:
        errors.append("%s:1: R-ADR-NAME: file name must be ADR-NNNN-<kebab-slug>.md" % label)
    try:
        meta = slgov.parse_front_matter(text)
    except slgov.FrontMatterError as exc:
        return errors + ["%s:1: R-ADR-FRONT: %s" % (label, exc)], None
    missing = [k for k in REQUIRED_KEYS if k not in meta]
    for key in missing:
        errors.append("%s:1: R-ADR-FRONT: front matter lacks '%s'" % (label, key))
    if missing:
        return errors, None
    if not ID_RE.match(str(meta["id"])):
        errors.append("%s:1: R-ADR-FRONT: id %r must look like ADR-0001" % (label, meta["id"]))
    elif m and meta["id"] != "ADR-" + m.group(1):
        errors.append("%s:1: R-ADR-NAME: file name number does not match id %s" % (label, meta["id"]))
    if meta["status"] not in STATUSES:
        errors.append("%s:1: R-ADR-FRONT: status %r must be one of %s" % (label, meta["status"], "|".join(STATUSES)))
    if not DATE_RE.match(str(meta["date"])):
        errors.append("%s:1: R-ADR-FRONT: date %r must look like 2026-10-17" % (label, meta["date"]))
    if not str(meta["title"]).strip():
        errors.append("%s:1: R-ADR-FRONT: title must not be empty" % label)
    for key in ("decision_refs", "supersedes"):
        value = meta[key]
        if not isinstance(value, list):
            errors.append("%s:1: R-ADR-FRONT: %s must be a list such as [a, b]" % (label, key))
        elif key == "decision_refs":
            for ref in value:
                if not REF_RE.match(ref):
                    errors.append(
                        "%s:1: R-ADR-FRONT: decision_refs entry %r must look like D-126 or ADR-0001" % (label, ref)
                    )
    errors += sections_error(text, label)
    errors += cmd_check_docs.citation_errors(text, label, CITATION_RE, "R-ADR-CITE")
    count = len(text.splitlines())
    if count > MAX_LINES:
        errors.append("%s:1: R-ADR-LENGTH: %d lines exceed the limit of %d" % (label, count, MAX_LINES))
    return errors, meta


def adr_errors(root):
    adr_dir = os.path.join(root, "docs", "adr")
    if not os.path.isdir(adr_dir):
        return ["docs/adr:1: R-ADR-0001: docs/adr is missing"]
    errors, metas = [], {}
    for name in sorted(os.listdir(adr_dir)):
        if name == "README.md" or not name.startswith("ADR-") or not name.endswith(".md"):
            continue
        try:
            with open(os.path.join(adr_dir, name), encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError) as exc:
            errors.append("docs/adr/%s:1: R-ADR-FRONT: unreadable: %s" % (name, exc))
            continue
        file_errors, meta = check_one(name, text)
        errors += file_errors
        if meta is not None and isinstance(meta.get("supersedes"), list):
            metas[name] = meta
    ids = {meta["id"]: meta for meta in metas.values()}
    for name, meta in sorted(metas.items()):
        for target in meta["supersedes"]:
            label = "docs/adr/%s:1: R-ADR-SUPERSEDE" % name
            if target == meta["id"]:
                errors.append("%s: %s supersedes itself" % (label, target))
            elif target not in ids:
                errors.append("%s: supersedes %s, which does not exist in docs/adr" % (label, target))
            elif ids[target]["status"] != "Superseded":
                errors.append(
                    "%s: %s is superseded by %s but its status is %s"
                    % (label, target, meta["id"], ids[target]["status"])
                )
    if "ADR-0001" not in ids:
        errors.append("docs/adr:1: R-ADR-0001: ADR-0001 (repository layout) is missing")
    log_path = os.path.join(root, *cmd_gen_adr_log.LOG_REL.split("/"))
    try:
        with open(log_path, encoding="utf-8") as fh:
            current = fh.read()
    except OSError:
        current = None
    if current != cmd_gen_adr_log.render(root):
        errors.append(
            "%s:1: R-ADR-LOG-STALE: stale or missing; run python3 -I tools/governance/cmd_gen_adr_log.py"
            % cmd_gen_adr_log.LOG_REL
        )
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_check_adr.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("adr-check", adr_errors(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
