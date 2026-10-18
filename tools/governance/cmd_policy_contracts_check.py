"""policy-contracts-check: the SL-GOV-008 sl-data block is well formed.

Rules (each failure line contains the rule id):
  R-POL-BLOCK      docs/gov/SL-GOV-008-dependency-policy.md has exactly one sl-data block holding one JSON object
  R-POL-CONTRACTS  contracts are C01..C12, unique and contiguous, each with text, enforced_by and episodes
  R-POL-ENFORCER   every enforced_by is `pending` or `just <recipe>` naming a recipe in the justfile
  R-POL-PROVIDERS  providers are the four known names; provider_order.ci and .dev are permutations of them
  R-POL-LICENSES   licenses_allowed is a non-empty list of unique strings
  R-POL-TIERS      tiers has non-empty entries A and B

Stdlib only (ADR-0004); run with `python3 -I cmd_policy_contracts_check.py [--root DIR]`.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

DOC_REL = "docs/gov/SL-GOV-008-dependency-policy.md"
PROVIDERS = ("vendored", "system", "cpm", "vcpkg")
CONTRACT_COUNT = 12
OPEN_RE = re.compile(r"^\s*```\s*sl-data\s*$")
CLOSE_RE = re.compile(r"^\s*```\s*$")
ENFORCER_RE = re.compile(r"^just ([A-Za-z_][\w-]*)$")


def sl_data_blocks(text):
    """Return the raw text of every ```sl-data fenced block."""
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


def load_policy(text):
    """Parse the one sl-data block. Returns (policy dict or None, errors)."""
    blocks = sl_data_blocks(text)
    if len(blocks) != 1:
        return None, ["%s:1: R-POL-BLOCK: expected exactly one sl-data block, found %d" % (DOC_REL, len(blocks))]
    try:
        data = json.loads(blocks[0])
    except ValueError as exc:
        return None, ["%s:1: R-POL-BLOCK: sl-data block is not valid JSON: %s" % (DOC_REL, exc)]
    if not isinstance(data, dict):
        return None, ["%s:1: R-POL-BLOCK: sl-data block must be a JSON object" % DOC_REL]
    return data, []


def contract_errors(policy, recipes):
    errors = []
    contracts = policy.get("contracts")
    if not isinstance(contracts, list):
        return ["%s:1: R-POL-CONTRACTS: 'contracts' must be a list" % DOC_REL]
    expected = ["C%02d" % i for i in range(1, CONTRACT_COUNT + 1)]
    ids = [c.get("id") if isinstance(c, dict) else None for c in contracts]
    if len(set(ids)) != len(ids):
        errors.append("%s:1: R-POL-CONTRACTS: duplicate contract ids %s" % (DOC_REL, ids))
    elif ids != expected:
        errors.append("%s:1: R-POL-CONTRACTS: ids must be C01..C%02d in order, found %s" % (DOC_REL, CONTRACT_COUNT, ids))
    for i, c in enumerate(contracts):
        if not isinstance(c, dict):
            errors.append("%s:1: R-POL-CONTRACTS: contract %d is not an object" % (DOC_REL, i + 1))
            continue
        cid = c.get("id")
        if not isinstance(c.get("text"), str) or not c["text"].strip():
            errors.append("%s:1: R-POL-CONTRACTS: %s has no text" % (DOC_REL, cid))
        eps = c.get("episodes")
        if not isinstance(eps, list) or not all(isinstance(e, int) and not isinstance(e, bool) and e > 0 for e in eps):
            errors.append("%s:1: R-POL-CONTRACTS: %s episodes must be a list of positive integers" % (DOC_REL, cid))
        by = c.get("enforced_by")
        if by == "pending":
            continue
        m = ENFORCER_RE.match(by) if isinstance(by, str) else None
        if not m:
            errors.append("%s:1: R-POL-ENFORCER: %s enforced_by %r must be 'pending' or 'just <recipe>'" % (DOC_REL, cid, by))
        elif m.group(1) not in recipes:
            errors.append("%s:1: R-POL-ENFORCER: %s names recipe '%s' which is not in the justfile" % (DOC_REL, cid, m.group(1)))
    return errors


def provider_errors(policy):
    errors = []
    providers = policy.get("providers")
    if not isinstance(providers, list) or sorted(providers) != sorted(PROVIDERS) or len(set(providers)) != len(providers):
        errors.append("%s:1: R-POL-PROVIDERS: providers must be exactly %s" % (DOC_REL, list(PROVIDERS)))
        return errors
    order = policy.get("provider_order")
    if not isinstance(order, dict):
        return errors + ["%s:1: R-POL-PROVIDERS: provider_order must be an object with ci and dev" % DOC_REL]
    for mode in ("ci", "dev"):
        lst = order.get(mode)
        if not isinstance(lst, list) or sorted(lst) != sorted(providers):
            errors.append("%s:1: R-POL-PROVIDERS: provider_order.%s %r is not a permutation of %s" % (DOC_REL, mode, lst, providers))
    return errors


def license_errors(policy):
    lic = policy.get("licenses_allowed")
    if not isinstance(lic, list) or not lic or not all(isinstance(x, str) and x.strip() for x in lic) or len(set(lic)) != len(lic):
        return ["%s:1: R-POL-LICENSES: licenses_allowed must be a non-empty list of unique strings" % DOC_REL]
    return []


def tier_errors(policy):
    tiers = policy.get("tiers")
    if not isinstance(tiers, dict) or not all(isinstance(tiers.get(k), str) and tiers[k].strip() for k in ("A", "B")):
        return ["%s:1: R-POL-TIERS: tiers must have non-empty string entries A and B" % DOC_REL]
    return []


def policy_errors(text, recipes):
    policy, errors = load_policy(text)
    if policy is None:
        return errors
    return contract_errors(policy, recipes) + provider_errors(policy) + license_errors(policy) + tier_errors(policy)


def run(root):
    try:
        with open(os.path.join(root, *DOC_REL.split("/")), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return ["%s:1: R-POL-BLOCK: unreadable: %s" % (DOC_REL, exc)]
    try:
        with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
            recipes = slgov.parse_justfile(fh.read())
    except (OSError, UnicodeDecodeError) as exc:
        return ["justfile:1: R-POL-ENFORCER: unreadable: %s" % exc]
    return policy_errors(text, recipes)


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_policy_contracts_check.py [--root DIR]", file=sys.stderr)
        return 2
    return slgov.report("policy-contracts-check", run(root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
