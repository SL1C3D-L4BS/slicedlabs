"""Unit tests for contracts-pending (cmd_contracts_pending.py): docs/gov and docs/arch sl-data contracts (WO-035)."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check  # noqa: E402
import cmd_contracts_pending  # noqa: E402
import slgov  # noqa: E402

REAL = slgov.ROOT
JUSTFILE = """real-recipe:
    @echo real

stub-recipe *args:
    @{{just_executable()}} stub stub-recipe
"""
RECIPES = {
    "schema": "sl-recipes/1",
    "note": "fixture",
    "recipes": {"real-recipe": {"owner": "WO-001", "status": "implemented"},
                "stub-recipe": {"owner": "WO-099", "status": "stub"}},
}


def doc(records):
    return "# fixture\n\n```sl-data\n%s\n```\n" % json.dumps({"invariants": records})


class ContractsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.write("justfile", JUSTFILE)
        self.write("tools/governance/recipes.json", json.dumps(RECIPES))

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, text):
        path = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def found(self):
        found, warnings = cmd_contracts_pending.pending_contracts(self.root)
        self.assertEqual(warnings, [])
        return dict(found)

    def test_arch_contracts_are_scanned(self):
        self.write("docs/arch/SL-ARCH-900-x.md", doc([{"id": "FAB-INV-X-01", "verified_by": "pending"}]))
        self.write("docs/arch/README.md", doc([{"id": "IGNORED", "verified_by": "pending"}]))
        self.assertEqual(self.found(), {"docs/arch/SL-ARCH-900-x.md FAB-INV-X-01 (verified_by)": "pending"})

    def test_gov_and_arch_are_both_listed_gov_first(self):
        self.write("docs/gov/SL-GOV-900-x.md", doc([{"id": "G1", "verified_by": "just stub-recipe"}]))
        self.write("docs/arch/SL-ARCH-900-x.md", doc([{"id": "A1", "enforced_by": "just nope"}]))
        found, _ = cmd_contracts_pending.pending_contracts(self.root)
        self.assertEqual(
            found,
            [
                ("docs/gov/SL-GOV-900-x.md G1 (verified_by)", "recipe stub-recipe is a stub (owner WO-099)"),
                ("docs/arch/SL-ARCH-900-x.md A1 (enforced_by)", "names unknown recipe nope"),
            ],
        )

    def test_implemented_recipe_with_existing_test_is_not_pending(self):
        self.write("tests/unit/fabric/x_test.cpp", "// test\n")
        records = [{"id": "A1", "verified_by": "just real-recipe area", "test": "tests/unit/fabric/x_test.cpp"}]
        self.write("docs/arch/SL-ARCH-900-x.md", doc(records))
        self.assertEqual(self.found(), {})

    def test_missing_test_file_is_pending_even_when_the_recipe_is_real(self):
        records = [{"id": "A1", "verified_by": "just real-recipe area", "test": "tests/unit/fabric/x_test.cpp"}]
        self.write("docs/arch/SL-ARCH-900-x.md", doc(records))
        self.assertEqual(self.found(), {"docs/arch/SL-ARCH-900-x.md A1 (verified_by)":
                                        "test tests/unit/fabric/x_test.cpp does not exist yet"})

    def test_stub_and_missing_test_reasons_combine(self):
        records = [{"id": "A1", "verified_by": "just stub-recipe", "test": "tests/unit/fabric/y_test.cpp"}]
        self.write("docs/arch/SL-ARCH-900-x.md", doc(records))
        reason = self.found()["docs/arch/SL-ARCH-900-x.md A1 (verified_by)"]
        self.assertEqual(reason, "recipe stub-recipe is a stub (owner WO-099); test tests/unit/fabric/y_test.cpp "
                                 "does not exist yet")

    def test_enforcement_prose_is_not_a_verifier(self):
        records = [{"id": "G1", "enforcement": "reviewed by the founder", "test": "tests/none.cpp"}]
        self.write("docs/gov/SL-GOV-900-x.md", doc(records))
        self.assertEqual(self.found(), {})

    def test_main_never_fails(self):
        self.write("docs/arch/SL-ARCH-900-x.md", doc([{"id": "A1", "verified_by": "pending"}]))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(cmd_contracts_pending.main(["x", "--root", self.root]), 0)
        self.assertIn("1 contract(s) pending", out.getvalue())


class RealArchContractsTest(unittest.TestCase):
    def test_every_arch_verifier_is_pending_or_an_existing_recipe(self):
        """SL-ARCH-001/002 invariants: verified_by/enforced_by is `pending` or `just <recipe>` of a real recipe."""
        state = cmd_contracts_pending.recipe_state(REAL)
        self.assertIn("fabric-test", state)
        seen = 0
        for name in ("SL-ARCH-001-canonical-project-model.md", "SL-ARCH-002-transaction-undo-recovery.md"):
            with open(os.path.join(REAL, "docs", "arch", name), encoding="utf-8") as fh:
                blocks, errors = cmd_constitution_check.sl_data_blocks(fh.read())
            self.assertEqual(errors, [])
            for b, block in enumerate(blocks):
                for where, node in cmd_contracts_pending._walk(block, "block%d" % b):
                    for key in ("verified_by", "enforced_by"):
                        if key not in node:
                            continue
                        seen += 1
                        reason = cmd_contracts_pending.classify(node[key], state) or ""
                        with self.subTest(name=name, id=node.get("id"), key=key):
                            self.assertNotIn("unknown recipe", reason)
                            self.assertTrue(node[key] == "pending" or node[key].startswith("just "), node[key])
        self.assertGreater(seen, 100)

    def test_real_tree_lists_arch_contracts(self):
        found, warnings = cmd_contracts_pending.pending_contracts(REAL)
        self.assertEqual(warnings, [])
        self.assertTrue(any(loc.startswith("docs/arch/SL-ARCH-001-") for loc, _ in found))
        self.assertTrue(any(loc.startswith("docs/arch/SL-ARCH-002-") for loc, _ in found))


if __name__ == "__main__":
    unittest.main()
