"""gov008-lock-check: the policy block of deps/deps.lock.json equals the SL-GOV-008 sl-data block.

Compared keys: providers, provider_order, licenses_allowed. When deps/deps.lock.json does not exist yet
(WO-017) it prints "gov008-lock-check: lock not present yet (WO-017)" and exits 0.

Rules (each failure line contains the rule id):
  R-LOCK-POLICY  the sl-data block cannot be read (see policy-contracts-check)
  R-LOCK-READ    the lock is unreadable, not JSON, or has no `policy` object
  R-LOCK-DRIFT   a compared key differs between the document and the lock

Stdlib only (ADR-0004); run with `python3 -I cmd_gov008_lock_check.py [--root DIR]`.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_policy_contracts_check as pcc  # noqa: E402
import slgov  # noqa: E402

LOCK_REL = "deps/deps.lock.json"
KEYS = ("providers", "provider_order", "licenses_allowed")


def drift_errors(policy, lock):
    """Errors for every compared key that differs. Lists compare in order, except licences (as sets)."""
    block = lock.get("policy") if isinstance(lock, dict) else None
    if not isinstance(block, dict):
        return ["%s:1: R-LOCK-READ: the lock has no 'policy' object" % LOCK_REL]
    errors = []
    for key in KEYS:
        want, have = policy.get(key), block.get(key)
        if key == "licenses_allowed" and isinstance(want, list) and isinstance(have, list):
            same = sorted(want) == sorted(have)
        else:
            same = want == have
        if not same:
            errors.append(
                "%s:1: R-LOCK-DRIFT: %s differs: %s has %s, lock has %s"
                % (LOCK_REL, key, pcc.DOC_REL, json.dumps(want), json.dumps(have))
            )
    return errors


def run(root):
    """Return (errors, lock_present)."""
    lock_path = os.path.join(root, *LOCK_REL.split("/"))
    if not os.path.exists(lock_path):
        return [], False
    try:
        with open(os.path.join(root, *pcc.DOC_REL.split("/")), encoding="utf-8") as fh:
            policy, errors = pcc.load_policy(fh.read())
    except (OSError, UnicodeDecodeError) as exc:
        return ["%s:1: R-LOCK-POLICY: unreadable: %s" % (pcc.DOC_REL, exc)], True
    if policy is None:
        return [e.replace("R-POL-BLOCK", "R-LOCK-POLICY") for e in errors], True
    try:
        with open(lock_path, encoding="utf-8") as fh:
            lock = json.load(fh)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        return ["%s:1: R-LOCK-READ: %s" % (LOCK_REL, exc)], True
    return drift_errors(policy, lock), True


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_gov008_lock_check.py [--root DIR]", file=sys.stderr)
        return 2
    errors, present = run(root)
    if not present:
        print("gov008-lock-check: lock not present yet (WO-017)")
        return 0
    return slgov.report("gov008-lock-check", errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
