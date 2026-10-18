"""check_fixture <bundle.json>...: run the registry rule engine (slib) over fixture bundles.

A bundle is {fixture, registry, documents?, work_orders?, plan_sha256?, views?}
(tests/governance/fixtures). Prints one `file: RULE path: message` line per finding.
Exit 0 when every bundle is clean, 1 when any finding exists, 2 on a load or usage error.
`--rules-only` prints just the sorted unique rule ids found (one per line) instead of findings.
`--list-rules` prints every rule id the engine knows. Run with `python3 -I`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slib  # noqa: E402


def main(argv):
    args = argv[1:]
    if args == ["--list-rules"]:
        print("\n".join(slib.RULES))
        return 0
    rules_only = False
    if args and args[0] == "--rules-only":
        rules_only, args = True, args[1:]
    if not args or any(a.startswith("--") for a in args):
        print("usage: cmd_check_fixture.py [--rules-only] <bundle.json>... | --list-rules", file=sys.stderr)
        return 2
    found_any = False
    rules = set()
    for path in args:
        try:
            findings = slib.check_bundle(slib.load_json(path))
        except slib.LoadError as exc:
            print("check_fixture: %s" % exc, file=sys.stderr)
            return 2
        for f in findings:
            found_any = True
            rules.add(f["rule"])
            if not rules_only:
                print("%s: %s" % (os.path.basename(path), slib.format_finding(f)))
    if rules_only:
        print("\n".join(sorted(rules)))
    return 1 if found_any else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
