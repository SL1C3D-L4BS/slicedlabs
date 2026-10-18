"""vol01-coverage: registry/vol01-documents.json fixes the 20 Volume 01 documents and the episode map.

Rules (each failure line starts with the file and ends with the rule id's message):
  R-V01-SHAPE     the file parses and has schema sl-vol01-documents/1, 'documents' and 'episodes' lists
  R-V01-DOCS      exactly the 20 expected document ids, each listed once
  R-V01-OWNER     each document has exactly one authoring_wo (a WO-NNN string, not a list, not null) and
                  work-orders/WO-NNN.md exists (WO-000 is the pre-factory baseline and has no file)
  R-V01-EPISODES  episodes 001-020 each appear exactly once, with one primary document from the 20 and
                  one existing work order

Stdlib only (ADR-0004); run with `python3 -I cmd_vol01_coverage.py [--root DIR]`.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

REL = "registry/vol01-documents.json"
SCHEMA = "sl-vol01-documents/1"
EXPECTED_IDS = (
    ["SL-GOV-%03d" % n for n in range(1, 11)]
    + ["SL-PLAN-001", "SL-PLAN-002", "SL-PLAN-004", "SL-PLAN-005"]
    + ["SL-REG-001", "SL-REG-002", "SL-REG-003", "SL-REG-004", "SL-REG-006"]
    + ["SL-VOL-01-SPEC"]
)
EXPECTED_EPISODES = ["SL-EP-%03d" % n for n in range(1, 21)]
WO_RE = re.compile(r"^WO-\d{3}$")
BASELINE_WO_WITHOUT_FILE = ("WO-000",)


def _wo_error(root, wo, where):
    if not isinstance(wo, str) or not WO_RE.match(wo):
        return "%s: R-V01-OWNER: %s must be exactly one work order id WO-NNN, found %r" % (REL, where, wo)
    if wo not in BASELINE_WO_WITHOUT_FILE and not os.path.isfile(os.path.join(root, "work-orders", wo + ".md")):
        return "%s: R-V01-OWNER: %s names %s but work-orders/%s.md does not exist" % (REL, where, wo, wo)
    return None


def coverage_errors(root):
    path = os.path.join(root, *REL.split("/"))
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        return ["%s: R-V01-SHAPE: %s" % (REL, exc)]
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        return ["%s: R-V01-SHAPE: schema must be %r" % (REL, SCHEMA)]
    docs, eps = data.get("documents"), data.get("episodes")
    if not isinstance(docs, list) or not isinstance(eps, list):
        return ["%s: R-V01-SHAPE: 'documents' and 'episodes' must be lists" % REL]
    errors = []
    ids = []
    for i, entry in enumerate(docs):
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            errors.append("%s: R-V01-SHAPE: documents[%d] needs an 'id' string" % (REL, i))
            continue
        ids.append(entry["id"])
        err = _wo_error(root, entry.get("authoring_wo"), "%s authoring_wo" % entry["id"])
        if err:
            errors.append(err)
    for doc_id in sorted(set(ids)):
        if ids.count(doc_id) > 1:
            errors.append("%s: R-V01-DOCS: %s is listed %d times (two owners)" % (REL, doc_id, ids.count(doc_id)))
    for doc_id in EXPECTED_IDS:
        if doc_id not in ids:
            errors.append("%s: R-V01-DOCS: %s is missing (no owner)" % (REL, doc_id))
    for doc_id in sorted(set(ids) - set(EXPECTED_IDS)):
        errors.append("%s: R-V01-DOCS: %s is not one of the 20 Volume 01 documents" % (REL, doc_id))
    seen = []
    for i, entry in enumerate(eps):
        if not isinstance(entry, dict) or not isinstance(entry.get("episode"), str):
            errors.append("%s: R-V01-SHAPE: episodes[%d] needs an 'episode' string" % (REL, i))
            continue
        ep = entry["episode"]
        seen.append(ep)
        if entry.get("document") not in EXPECTED_IDS:
            errors.append(
                "%s: R-V01-EPISODES: %s primary document %r is not one of the 20" % (REL, ep, entry.get("document"))
            )
        err = _wo_error(root, entry.get("wo"), "%s wo" % ep)
        if err:
            errors.append(err.replace("R-V01-OWNER", "R-V01-EPISODES"))
    for ep in sorted(set(seen)):
        if seen.count(ep) > 1:
            errors.append("%s: R-V01-EPISODES: %s appears %d times (two primary documents)" % (REL, ep, seen.count(ep)))
    for ep in EXPECTED_EPISODES:
        if ep not in seen:
            errors.append("%s: R-V01-EPISODES: %s has no primary document" % (REL, ep))
    for ep in sorted(set(seen) - set(EXPECTED_EPISODES)):
        errors.append("%s: R-V01-EPISODES: %s is outside 001-020" % (REL, ep))
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_vol01_coverage.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("vol01-coverage", coverage_errors(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
