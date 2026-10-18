"""stub <recipe>: print `stub: owner WO-0xx` from recipes.json and exit 3 (not implemented yet).
Exit 2 when the recipe is unknown. Run with `python3 -I`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

EXIT_STUB = 3


def main(argv):
    if len(argv) != 2:
        print("usage: cmd_stub.py <recipe>", file=sys.stderr)
        return 2
    manifest = slgov.load_recipes_json(os.path.join(slgov.ROOT, "tools", "governance", "recipes.json"))
    entry = manifest.get(argv[1])
    if entry is None:
        print("stub: unknown recipe '%s' (not in tools/governance/recipes.json)" % argv[1], file=sys.stderr)
        return 2
    print("stub: owner %s" % entry["owner"])
    return EXIT_STUB


if __name__ == "__main__":
    sys.exit(main(sys.argv))
