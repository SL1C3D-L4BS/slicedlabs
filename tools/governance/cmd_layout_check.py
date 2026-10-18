"""layout-check: the ADR-0001 directories exist with a README, no tracked 0-byte files or submodules.

Usage: git ls-files -z --cached --others --exclude-standard | python3 -I cmd_layout_check.py
(`python3 -I` puts neither the cwd nor the script directory on sys.path, so add it explicitly.)
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402


def main():
    with open(os.path.join(slgov.ROOT, "tools", "governance", "layout.json"), encoding="utf-8") as fh:
        layout = json.load(fh)
    files = [p for p in sys.stdin.buffer.read().decode("utf-8").split("\0") if p]
    errors = slgov.layout_errors(slgov.ROOT, layout, files)
    for d in layout.get("deferred", []):
        if not os.path.isdir(os.path.join(slgov.ROOT, d)):
            print("layout-check: note: %s is deferred (needs a founder path grant)" % d)
    return slgov.report("layout-check", errors)


if __name__ == "__main__":
    sys.exit(main())
