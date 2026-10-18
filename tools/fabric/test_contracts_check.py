"""Tests for the fabric contracts checker (WO-036): control fixture plus one planted defect per rule.

Fixtures live in tests/governance/fabric/contracts/: `control/` is a complete mini repository (two ARCH documents,
work orders, a justfile, recipes.json, a C++ and a Python test); every other directory is an overlay copied on top of
the control, named after the rule it plants (r-fab-dup plants R-FAB-DUP).
"""

import contextlib
import importlib.util
import io
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("contracts_check", os.path.join(_here, "contracts_check.py"))
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)

REPO = os.path.dirname(os.path.dirname(_here))
FIXTURES = os.path.join(REPO, "tests", "governance", "fabric", "contracts")
# (overlay directory, rule it plants, id the error line must name)
ERRORS = (
    ("r-fab-shape", "R-FAB-SHAPE", "FAB-INV-ID-01"),
    ("r-fab-dup", "R-FAB-DUP", "FAB-INV-ID-01"),
    ("r-fab-rule", "R-FAB-RULE", "FAB-INV-ID-01"),
    ("r-fab-wo", "R-FAB-WO", "FAB-INV-ID-01"),
    ("r-fab-orphan-test", "R-FAB-ORPHAN-TEST", "FAB-INV-ZZ-99"),
    ("r-fab-location", "R-FAB-LOCATION", "FAB-INV-ID-01"),
    ("r-fab-recipe", "R-FAB-RECIPE", "FAB-INV-ID-01"),
)
# pending in the default mode, an error in --strict
STRICT_ONLY = (
    ("r-fab-missing-test", "R-FAB-MISSING-TEST", "FAB-INV-ID-02"),
    ("r-fab-recipe-stub", "R-FAB-RECIPE", "FAB-INV-ID-01"),
)


def run(root, strict=False):
    """Run the checker CLI on root; return (exit status, stdout)."""
    out = io.StringIO()
    argv = ["contracts_check.py", "--root", root] + (["--strict"] if strict else [])
    with contextlib.redirect_stdout(out):
        status = cc.main(argv)
    return status, out.getvalue()


@contextlib.contextmanager
def tree(overlay=None):
    """A temporary copy of the control fixture, with an overlay directory copied on top."""
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copytree(os.path.join(FIXTURES, "control"), tmp, dirs_exist_ok=True)
        if overlay:
            shutil.copytree(os.path.join(FIXTURES, overlay), tmp, dirs_exist_ok=True)
        yield tmp


class ContractsCheckTest(unittest.TestCase):
    def named(self, out, rule, fid):
        return [line for line in out.splitlines() if rule in line and fid in line and not line.startswith("pending")]

    def test_fab_inv_cf_01_bijection(self):
        with tree() as root:
            for strict in (False, True):
                status, out = run(root, strict)
                self.assertEqual(status, 0, out)
                self.assertTrue(out.rstrip().endswith("invariants 3 bound 3 pending 0"), out)
        for overlay, rule, fid in ERRORS:
            with tree(overlay) as root:
                for strict in (False, True):
                    status, out = run(root, strict)
                    self.assertEqual(status, 1, "%s strict=%s: %s" % (overlay, strict, out))
                    self.assertTrue(self.named(out, rule, fid), "%s: no line names %s and %s:\n%s" % (
                        overlay, rule, fid, out))
        for overlay, rule, fid in STRICT_ONLY:
            with tree(overlay) as root:
                status, out = run(root)
                self.assertEqual(status, 0, "%s must be pending, not an error: %s" % (overlay, out))
                self.assertIn("pending %s" % rule, out)
                self.assertIn(fid, out)
                self.assertTrue(out.rstrip().endswith("pending 1"), out)
                status, out = run(root, strict=True)
                self.assertEqual(status, 1, out)
                self.assertTrue(self.named(out, rule, fid), "%s strict: %s" % (overlay, out))

    def test_retired_ids_are_listed_and_orphan_tests_fail(self):
        with tree() as root:
            doc = os.path.join(root, "docs", "arch", "SL-ARCH-001-fixture.md")
            with open(doc, encoding="utf-8") as fh:
                text = fh.read()
            text = text.replace('"retired": []', '"retired": ["FAB-INV-BY-01"]')
            text = text.replace('"id": "FAB-INV-BY-01"', '"id": "FAB-INV-BY-02"')
            with open(doc, "w", encoding="utf-8") as fh:
                fh.write(text)
            status, out = run(root)
            self.assertEqual(status, 1)
            self.assertTrue(self.named(out, "R-FAB-ORPHAN-TEST", "FAB-INV-BY-01"), out)
            self.assertIn("retired", out)

    def test_a_missing_document_is_a_shape_error(self):
        with tree() as root:
            os.remove(os.path.join(root, "docs", "arch", "SL-ARCH-002-fixture.md"))
            status, out = run(root)
            self.assertEqual(status, 1)
            self.assertIn("R-FAB-SHAPE: docs/arch/SL-ARCH-002-", out)

    def test_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cc.main(["contracts_check.py", "--nope"]), 2)

    def test_repository_documents_are_consistent(self):
        errors, pending, (total, bound, waiting) = cc.check(REPO)
        self.assertEqual(errors, [])
        self.assertGreater(total, 0)
        self.assertEqual(bound + waiting, total)
        self.assertEqual(len({p[2] for p in pending}), waiting)


if __name__ == "__main__":
    unittest.main()
