"""contracts-pending: list the `sl-data` contracts in docs/gov and docs/arch that no gate verifies yet. Never fails.

A contract is any object inside an `sl-data` block of docs/gov/*.md or docs/arch/*.md that has a `verified_by`,
`enforced_by` or `enforcement` key. It is listed when that value is `pending`, or names `just <recipe>` (or is
exactly a recipe name) whose recipe is still a stub in the justfile or tools/governance/recipes.json, or is not a
recipe at all, or when the contract names a `test` file that does not exist yet (the docs/arch invariants carry
one: the recipe can be real before the test lands). Descriptive enforcement prose is not a verifier and is not
listed. The exit code is 0 whatever the list holds (SL-GOV-008, SL-GOV-009 and SL-ARCH-001/002 add contracts as
their gates land). Stdlib only (ADR-0004); run with `python3 -I cmd_contracts_pending.py [--root DIR]`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check  # noqa: E402
import slgov  # noqa: E402

KEYS = ("verified_by", "enforced_by", "enforcement")
DOC_DIRS = ("docs/gov", "docs/arch")
_JUST = re.compile(r"\bjust\s+([A-Za-z_][\w-]*)")
_NAME = re.compile(r"^[A-Za-z_][\w-]*$")


def recipe_state(root):
    """Map recipe name -> (is_stub, owner). Empty when the justfile cannot be read."""
    state = {}
    try:
        with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
            recipes = slgov.parse_justfile(fh.read())
    except OSError:
        return state
    try:
        manifest = slgov.load_recipes_json(os.path.join(root, "tools", "governance", "recipes.json"))
    except (OSError, ValueError, KeyError):
        manifest = {}
    for name, recipe in recipes.items():
        owner = manifest.get(name, {}).get("owner", "?")
        is_stub = slgov.stub_target(recipe) is not None or manifest.get(name, {}).get("status") == "stub"
        state[name] = (is_stub, owner)
    return state


def classify(value, state):
    """Return a reason string when `value` is not yet verified, else None."""
    texts = value if isinstance(value, list) else [value]
    reasons = []
    for text in texts:
        if not isinstance(text, str):
            continue
        s = text.strip()
        if s.lower().startswith("pending"):
            reasons.append("pending")
            continue
        names = _JUST.findall(s) or ([s] if _NAME.match(s) and s in state else [])
        for name in names:
            if name not in state:
                reasons.append("names unknown recipe %s" % name)
            elif state[name][0]:
                reasons.append("recipe %s is a stub (owner %s)" % (name, state[name][1]))
    return "; ".join(reasons) or None


def _walk(node, where):
    if isinstance(node, dict):
        if any(k in node for k in KEYS):
            yield where, node
        for k, v in node.items():
            yield from _walk(v, "%s.%s" % (where, k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk(v, "%s[%d]" % (where, i))


def _missing_test(root, node):
    """A reason when the contract names a `test` path that is not a file under root, else None."""
    test = node.get("test")
    if not isinstance(test, str) or not test.strip():
        return None
    if os.path.isfile(os.path.join(root, *test.strip().split("/"))):
        return None
    return "test %s does not exist yet" % test.strip()


def contract_files(root):
    """Yield (relative path, absolute path) of every SL-*.md in DOC_DIRS, sorted per directory."""
    for rel_dir in DOC_DIRS:
        directory = os.path.join(root, *rel_dir.split("/"))
        names = sorted(os.listdir(directory)) if os.path.isdir(directory) else []
        for name in names:
            if name.startswith("SL-") and name.endswith(".md"):
                yield "%s/%s" % (rel_dir, name), os.path.join(directory, name)


def pending_contracts(root):
    """Return (list of (location, reason), warnings)."""
    state = recipe_state(root)
    found, warnings = [], []
    for rel, full in contract_files(root):
        try:
            with open(full, encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError) as exc:
            warnings.append("%s: unreadable: %s" % (rel, exc))
            continue
        blocks, errors = cmd_constitution_check.sl_data_blocks(text)
        warnings += ["%s: %s" % (rel, e) for e in errors]
        for b, block in enumerate(blocks):
            for where, node in _walk(block, "block%d" % b):
                label = node.get("id") or node.get("area") or node.get("item") or where
                for key in KEYS:
                    if key in node:
                        reason = classify(node[key], state)
                        if key != "enforcement":
                            reason = "; ".join(r for r in (reason, _missing_test(root, node)) if r) or None
                        if reason:
                            found.append(("%s %s (%s)" % (rel, label, key), reason))
    return found, warnings


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_contracts_pending.py [--root DIR]", file=sys.stderr)
        return 2
    found, warnings = pending_contracts(root)
    for w in warnings:
        print("contracts-pending: warning: " + w)
    for location, reason in found:
        print("pending: %s: %s" % (location, reason))
    print("contracts-pending: %d contract(s) pending (informational, never fails)" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
