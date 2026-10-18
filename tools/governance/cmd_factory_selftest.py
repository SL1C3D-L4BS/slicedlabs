"""factory-selftest: recipes.json and the justfile agree, and no recipe in the closure of `fast` and
`check` is a stub whose owner work order has landed. Run with `python3 -I`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402


def main():
    with open(os.path.join(slgov.ROOT, "justfile"), encoding="utf-8") as fh:
        recipes = slgov.parse_justfile(fh.read())
    manifest = slgov.load_recipes_json(os.path.join(slgov.ROOT, "tools", "governance", "recipes.json"))
    landed = slgov.landed_work_orders(slgov.ROOT)
    return slgov.report("factory-selftest", slgov.selftest_errors(recipes, manifest, landed))


if __name__ == "__main__":
    sys.exit(main())
