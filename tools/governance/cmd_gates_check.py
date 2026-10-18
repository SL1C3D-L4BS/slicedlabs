"""gates-check: FACTORY.md parses, gate commands are existing recipes, .gitignore carries the stub
block, and no recipe or script fetches from the network. Run with `python3 -I`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402


def main():
    return slgov.report("gates-check", slgov.gate_errors(slgov.ROOT))


if __name__ == "__main__":
    sys.exit(main())
