"""Unit tests for plan-check R-PLAN-VOL04 and the table-driven volume specs (WO-035).

The WO-011 plan-check tests (R-PLAN-EP, GATE, GRAPH, VOL03, KOHI, CLEAN-ROOM) stay in test_views.py.
"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_plan_check  # noqa: E402
import cmd_views  # noqa: E402
import slgov  # noqa: E402

REAL = slgov.ROOT
VOL03 = "docs/vol/SL-VOL-03-SPEC.md"
VOL04 = "docs/vol/SL-VOL-04-SPEC.md"
REC062 = '"work_orders": ["WO-035", "WO-039", "WO-045"], "primary_wo": "WO-039", "acceptance": "just fabric-test type"'


class Tree(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.copy(cmd_views.REGISTRY_REL)
        for rel in cmd_plan_check.DOCS.values():
            self.copy(rel)
        for name in os.listdir(os.path.join(REAL, "work-orders")):
            if name.endswith(".md"):
                self.copy("work-orders/" + name)
        cmd_views.write_views(self.root)

    def tearDown(self):
        self._tmp.cleanup()

    def copy(self, rel):
        dst = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(REAL, *rel.split("/")), dst)

    def path(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def edit(self, rel, old, new):
        """Replace the first occurrence of old (which must be present)."""
        with open(self.path(rel), encoding="utf-8", newline="") as fh:
            text = fh.read()
        self.assertIn(old, text)
        with open(self.path(rel), "w", encoding="utf-8", newline="") as fh:
            fh.write(text.replace(old, new, 1))

    def edit_record(self, eid, old, new):
        """Replace old with new on the SL-VOL-04-SPEC record line of episode eid."""
        with open(self.path(VOL04), encoding="utf-8", newline="") as fh:
            lines = fh.read().split("\n")
        hits = [i for i, ln in enumerate(lines) if '"episode": "%s"' % eid in ln]
        self.assertEqual(len(hits), 1)
        self.assertIn(old, lines[hits[0]])
        lines[hits[0]] = lines[hits[0]].replace(old, new, 1)
        with open(self.path(VOL04), "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines))

    def errors(self):
        return cmd_plan_check.plan_errors(self.root)

    def rules(self):
        return sorted({e.split(": ")[1] for e in self.errors()})

    def assert_vol04(self, *needles):
        """Every error is R-PLAN-VOL04 in SL-VOL-04-SPEC, and one error contains all needles."""
        errors = self.errors()
        self.assertTrue(errors, "expected R-PLAN-VOL04 errors")
        self.assertEqual(self.rules(), ["R-PLAN-VOL04"], errors)
        self.assertTrue(all(e.startswith(VOL04 + ":1: R-PLAN-VOL04: ") for e in errors), errors)
        self.assertTrue(any(all(n in e for n in needles) for e in errors), (needles, errors))
        return errors


class Vol04Test(Tree):
    def test_clean_copy_passes(self):
        self.assertEqual(self.errors(), [])

    def test_real_tree_passes(self):
        self.assertEqual(cmd_plan_check.plan_errors(REAL), [])

    def test_specs_are_table_driven(self):
        rows = {s["rule"]: s for s in cmd_plan_check.VOLUME_SPECS}
        self.assertEqual(sorted(rows), ["R-PLAN-VOL03", "R-PLAN-VOL04"])
        self.assertEqual((rows["R-PLAN-VOL04"]["first"], rows["R-PLAN-VOL04"]["last"]), (61, 80))
        self.assertTrue(rows["R-PLAN-VOL04"]["primary"] and not rows["R-PLAN-VOL03"]["primary"])

    def test_missing_record(self):
        path = self.path(VOL04)
        with open(path, encoding="utf-8") as fh:
            lines = [ln for ln in fh.read().split("\n") if '"SL-EP-070"' not in ln]
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        self.assert_vol04("records must cover exactly SL-EP-061 to SL-EP-080", "SL-EP-070")

    def test_record_out_of_range(self):
        self.edit(VOL04, '"episode": "SL-EP-080"', '"episode": "SL-EP-081"')
        self.assert_vol04("unexpected ['SL-EP-081']")

    def test_title_and_purpose_must_equal_registry(self):
        self.edit(VOL04, '"title": "Canonical type system"', '"title": "Type system"')
        self.assert_vol04("SL-EP-062 title", "differs from the registry")
        self.edit(VOL04, '"purpose": "approved schemas plus executable contract tests"', '"purpose": "schemas"')
        self.assert_vol04("SL-EP-080 purpose", "differs from the registry")

    def test_required_keys(self):
        keys = ("work_orders", "primary_wo", "acceptance", "kohi_ref", "seam", "performance", "release")
        self.assertEqual(cmd_plan_check.VOLUME_SPECS[1]["keys"], keys)
        for key in ("seam", "performance", "release"):
            self.edit_record("SL-EP-073", '"%s": ' % key, '"%s_": ' % key)
        for key in ("seam", "performance", "release"):
            self.assert_vol04("SL-EP-073 lacks '%s'" % key)

    def test_kohi_ref_must_equal_registry(self):
        self.edit_record("SL-EP-069", '"kohi_ref": "none"', '"kohi_ref": "kohi-07"')
        self.assert_vol04("SL-EP-069 kohi_ref differs from the registry")

    def test_primary_not_in_work_orders(self):
        self.edit(VOL04, REC062, REC062.replace('"primary_wo": "WO-039"', '"primary_wo": "WO-036"'))
        self.assert_vol04("SL-EP-062 primary_wo WO-036 is not in its work_orders")

    def test_primary_must_be_one_id(self):
        self.edit(VOL04, REC062, REC062.replace('"primary_wo": "WO-039"', '"primary_wo": ["WO-039", "WO-045"]'))
        self.assert_vol04("SL-EP-062 primary_wo must be exactly one work order id")

    def test_acceptance_absent_from_primary(self):
        self.edit(VOL04, REC062, REC062.replace("just fabric-test type", "just fabric-test codec"))
        self.assert_vol04("SL-EP-062 acceptance 'just fabric-test codec' is not in the front-matter acceptance",
                          "its primary_wo WO-039")

    def test_acceptance_recipe_must_exist(self):
        self.edit(VOL04, REC062, REC062.replace("just fabric-test type", "just no-such-recipe"))
        self.assert_vol04("SL-EP-062 acceptance 'just no-such-recipe' is not `just <recipe>` for a recipe in the "
                          "justfile")

    def test_tracing_work_order_must_be_listed(self):
        self.edit(VOL04, REC062, REC062.replace('"WO-035", "WO-039", "WO-045"', '"WO-035", "WO-039"'))
        self.assert_vol04("WO-045 traces SL-EP-062 but the SL-EP-062 record does not list it")

    def test_listed_work_order_must_trace(self):
        self.edit_record("SL-EP-062", '"WO-045"]', '"WO-045", "WO-044"]')
        self.assert_vol04("SL-EP-062 lists WO-044 but its Traces line does not name SL-EP-062")

    def test_unknown_work_order(self):
        self.edit_record("SL-EP-062", '"WO-045"]', '"WO-045", "WO-999"]')
        self.assert_vol04("SL-EP-062 lists WO-999 which has no file in work-orders/")

    def test_a_work_order_is_primary_for_at_most_one_episode(self):
        self.edit("work-orders/WO-039.md", "Traces: ", "Traces: SL-EP-061 ")
        self.edit(VOL04, '"work_orders": ["WO-035", "WO-037", "WO-058", "WO-062"], "primary_wo": "WO-037"',
                  '"work_orders": ["WO-035", "WO-037", "WO-039", "WO-058", "WO-062"], "primary_wo": "WO-039"')
        self.edit(VOL04, '"acceptance": "just fabric-test id"', '"acceptance": "just fabric-test value"')
        self.assert_vol04("WO-039 is primary_wo of more than one episode (SL-EP-061, SL-EP-062)")

    def test_unparsable_spec(self):
        self.edit(VOL04, '"volume": "SL-VOL-04",', '"volume": "SL-VOL-04",,')
        self.assert_vol04("no parsable sl-data block")

    def test_missing_justfile_is_a_doc_error(self):
        os.remove(self.path("justfile"))
        self.assertEqual(self.rules(), ["R-PLAN-DOC"])

    def test_justfile_is_not_scanned_for_episode_ids(self):
        with open(self.path("justfile"), "a", encoding="utf-8") as fh:
            fh.write("\n# SL-EP-9999 in a comment\n")
        self.assertEqual(self.errors(), [])


class Vol03UnchangedTest(Tree):
    """R-PLAN-VOL03 messages are byte-identical to WO-011 after the table-driven rewrite."""

    def test_wrong_work_order_messages(self):
        self.edit(VOL03, '"work_orders": ["WO-015", "WO-021"]', '"work_orders": ["WO-016", "WO-021"]')
        self.assertEqual(
            self.errors(),
            [
                VOL03 + ":1: R-PLAN-VOL03: SL-EP-043 lists WO-016 but its Traces line does not name SL-EP-043",
                VOL03 + ":1: R-PLAN-VOL03: WO-015 traces SL-EP-043 but the SL-EP-043 record does not list it",
            ],
        )

    def test_missing_record_message(self):
        path = self.path(VOL03)
        with open(path, encoding="utf-8") as fh:
            lines = [ln for ln in fh.read().split("\n") if '"SL-EP-059"' not in ln]
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        self.assertEqual(
            self.errors(),
            [
                VOL03 + ":1: R-PLAN-VOL03: records must cover exactly SL-EP-041 to SL-EP-060 in order (missing "
                "['SL-EP-059']; unexpected none)"
            ],
        )

    def test_title_and_key_messages(self):
        self.edit(VOL03, "Automated formatting", "Auto formatting")
        errors = self.errors()
        self.assertEqual(len(errors), 1)
        self.assertRegex(errors[0], r"^docs/vol/SL-VOL-03-SPEC\.md:1: R-PLAN-VOL03: SL-EP-0\d\d title 'Auto formatting")

    def test_vol03_needs_no_primary_wo(self):
        self.assertNotIn("primary_wo", cmd_plan_check.VOLUME_SPECS[0]["keys"])
        self.assertEqual(self.errors(), [])


if __name__ == "__main__":
    unittest.main()
