"""test-report: turn ctest's JUnit file into a Markdown summary with per-tier counts and durations (WO-026).

Usage: python3 -I docs/testing/test_report.py <build-dir>
Reads <build-dir>/test-report.xml (ctest --output-junit) and <build-dir>/test-registry.tsv (tier, name, timeout, written
by sl_testing_finalize), writes <build-dir>/test-report.md. Exit 0 when the report was written, 2 when an input is
missing.
"""

import os
import sys
import xml.etree.ElementTree as ET

TIERS = ("unit", "integration", "system", "e2e")


def main(argv):
    if len(argv) != 2:
        print("usage: test_report.py <build-dir>", file=sys.stderr)
        return 2
    build = argv[1]
    junit = os.path.join(build, "test-report.xml")
    registry = os.path.join(build, "test-registry.tsv")
    for path in (junit, registry):
        if not os.path.isfile(path):
            print("test-report: missing %s" % path, file=sys.stderr)
            return 2

    tier_of = {}
    with open(registry, encoding="utf-8") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 3:
                tier_of[parts[1]] = parts[0]

    rows = []
    for case in ET.parse(junit).getroot().iter("testcase"):
        name = case.get("name", "?")
        ok = case.get("status") == "run" and case.find("failure") is None
        rows.append((tier_of.get(name, "other"), name, ok, float(case.get("time", "0"))))

    lines = ["# Test report", "", "| tier | tests | passed | failed | seconds |", "|---|---:|---:|---:|---:|"]
    total = [0, 0, 0, 0.0]
    for tier in TIERS + ("other",):
        sel = [r for r in rows if r[0] == tier]
        if not sel and tier == "other":
            continue
        counts = [len(sel), sum(1 for r in sel if r[2]), sum(1 for r in sel if not r[2]), sum(r[3] for r in sel)]
        total = [a + b for a, b in zip(total, counts)]
        lines.append("| %s | %d | %d | %d | %.2f |" % ((tier,) + tuple(counts)))
    lines.append("| **total** | %d | %d | %d | %.2f |" % tuple(total))
    lines += ["", "| tier | test | result | seconds |", "|---|---|---|---:|"]
    for tier, name, ok, secs in sorted(rows, key=lambda r: (r[0], r[1])):
        lines.append("| %s | %s | %s | %.2f |" % (tier, name, "pass" if ok else "FAIL", secs))
    with open(os.path.join(build, "test-report.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print(
        "test-report: %d tests, %d failed; %s, %s" % (total[0], total[2], junit, os.path.join(build, "test-report.md"))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
