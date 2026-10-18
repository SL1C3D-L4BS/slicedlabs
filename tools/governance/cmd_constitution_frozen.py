"""constitution-frozen: once the founder approves SL-GOV-001 the Constitution is frozen (episode 020, WO-023).

Before approval (registry/SL-REG-001.approvals.json has no SL-GOV-001 entry) the check prints
"constitution-frozen: not yet approved (episode 020 pending)" and exits 0. With an entry:
  R-FROZEN-SHA     the sha256 of docs/gov/SL-GOV-001-constitution.md equals the recorded sha256
  R-FROZEN-STATUS  the front matter status is Approved (and its version equals the recorded version, when recorded)
  R-FROZEN-LOCKED  the sl-data `locked` block equals the master plan section 1.5 table (cmd_constitution_check)

Stdlib only (ADR-0004); run with `python3 -I cmd_constitution_frozen.py [--root DIR]`.
"""

import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check as cc  # noqa: E402
import cmd_gen_docs  # noqa: E402
import slgov  # noqa: E402

DOC_ID = "SL-GOV-001"
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
LABEL = "constitution-frozen"
PENDING = "constitution-frozen: not yet approved (episode 020 pending)"


def frozen_errors(root):
    """Return (pending, errors). pending is True when SL-GOV-001 has no approvals entry."""
    approvals, err = cmd_gen_docs.load_approvals(root)
    where = cmd_gen_docs.APPROVALS_REL
    if err:
        return False, ["R-FROZEN-SHA: " + err]
    if DOC_ID not in approvals:
        return True, []
    entry = approvals[DOC_ID]
    if not isinstance(entry, dict) or not isinstance(entry.get("sha256"), str) or not SHA_RE.match(entry["sha256"]):
        return False, ["%s[%s]: R-FROZEN-SHA: entry must be an object with a 64-hex sha256" % (where, DOC_ID)]
    path = os.path.join(root, *cc.CONSTITUTION_REL.split("/"))
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
        text = raw.decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        return False, ["%s: R-FROZEN-SHA: %s" % (cc.CONSTITUTION_REL, exc)]
    errors = []
    actual = hashlib.sha256(raw).hexdigest()
    if actual != entry["sha256"]:
        errors.append(
            "%s: R-FROZEN-SHA: sha256 is %s, %s records %s (the Constitution changed after approval)"
            % (cc.CONSTITUTION_REL, actual, where, entry["sha256"])
        )
    try:
        meta = slgov.parse_front_matter(text)
    except slgov.FrontMatterError as exc:
        meta = None
        errors.append("%s:1: R-FROZEN-STATUS: %s" % (cc.CONSTITUTION_REL, exc))
    if meta is not None:
        if meta.get("status") != "Approved":
            errors.append(
                "%s:1: R-FROZEN-STATUS: front matter status is %r, must be Approved"
                % (cc.CONSTITUTION_REL, meta.get("status"))
            )
        recorded = entry.get("version")
        if isinstance(recorded, str) and recorded and meta.get("version") != recorded:
            errors.append(
                "%s:1: R-FROZEN-STATUS: front matter version is %r, %s records %r"
                % (cc.CONSTITUTION_REL, meta.get("version"), where, recorded)
            )
    plan, perr = cc._read(root, cc.PLAN_REL)
    if perr:
        errors.append("R-FROZEN-LOCKED: " + perr)
    else:
        errors += [e.replace("R-CONST-LOCKED", "R-FROZEN-LOCKED") for e in cc.locked_errors(text, plan)]
    return False, errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_constitution_frozen.py [--root DIR]", file=sys.stderr)
        return 2
    pending, errors = frozen_errors(root)
    if pending:
        print(PENDING)
        return 0
    return slgov.report(LABEL, errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
