"""Unit tests for plan-check R-PLAN-VOL04, R-PLAN-VOL05 (WO-075) and the table-driven volume specs (WO-035).

The WO-011 plan-check tests (R-PLAN-EP, GATE, GRAPH, VOL03, KOHI, CLEAN-ROOM) stay in test_views.py.
"""

import json
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
VOL05 = "docs/vol/SL-VOL-05-SPEC.md"


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

    def edit_record(self, eid, old, new, rel=VOL04):
        """Replace old with new on the specification record line of episode eid (SL-VOL-04-SPEC by default)."""
        with open(self.path(rel), encoding="utf-8", newline="") as fh:
            lines = fh.read().split("\n")
        hits = [i for i, ln in enumerate(lines) if '"episode": "%s"' % eid in ln]
        self.assertEqual(len(hits), 1)
        self.assertIn(old, lines[hits[0]])
        lines[hits[0]] = lines[hits[0]].replace(old, new, 1)
        with open(self.path(rel), "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines))

    def record(self, rel, eid):
        """The record of episode eid in the specification file under test, derived rather than hardcoded.

        Returns the parsed record plus `wos_text` (its work_orders list as written), `head` (the work_orders,
        primary_wo and acceptance span as written) and `tracing` (the work orders whose Traces line names eid).
        """
        with open(self.path(rel), encoding="utf-8") as fh:
            hits = [ln for ln in fh.read().split("\n") if '"episode": "%s"' % eid in ln]
        self.assertEqual(len(hits), 1, eid)
        rec = json.loads(hits[0].strip().rstrip(","))
        rec["wos_text"] = '"work_orders": %s' % json.dumps(rec["work_orders"])
        rec["head"] = '%s, "primary_wo": %s, "acceptance": %s' % (
            rec["wos_text"],
            json.dumps(rec["primary_wo"]),
            json.dumps(rec["acceptance"]),
        )
        self.assertIn(rec["head"], hits[0])
        tr = cmd_plan_check.traces(self.root)
        rec["tracing"] = sorted(wo for wo, eps in tr.items() if eid in eps)
        return rec

    def tracing_non_primary(self, rec):
        """A listed work order that traces the episode and is not its primary_wo."""
        candidates = [wo for wo in rec["work_orders"] if wo in rec["tracing"] and wo != rec["primary_wo"]]
        self.assertTrue(candidates, rec["episode"])
        return candidates[-1]

    def work_order_not_tracing(self, eid):
        """A work order with a file in work-orders/ whose Traces line does not name eid."""
        tr = cmd_plan_check.traces(self.root)
        candidates = sorted(wo for wo, eps in tr.items() if eid not in eps)
        self.assertTrue(candidates, eid)
        return candidates[-1]

    def unknown_work_order(self):
        """A work order id with no file in work-orders/."""
        tr = cmd_plan_check.traces(self.root)
        return "WO-%03d" % (max(int(wo[3:]) for wo in tr) + 1)

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

    def assert_vol05(self, *needles):
        """Every error is R-PLAN-VOL05 in SL-VOL-05-SPEC, and one error contains all needles."""
        errors = self.errors()
        self.assertTrue(errors, "expected R-PLAN-VOL05 errors")
        self.assertEqual(self.rules(), ["R-PLAN-VOL05"], errors)
        self.assertTrue(all(e.startswith(VOL05 + ":1: R-PLAN-VOL05: ") for e in errors), errors)
        self.assertTrue(any(all(n in e for n in needles) for e in errors), (needles, errors))
        return errors


class Vol04Test(Tree):
    def test_clean_copy_passes(self):
        self.assertEqual(self.errors(), [])

    def test_real_tree_passes(self):
        self.assertEqual(cmd_plan_check.plan_errors(REAL), [])

    def test_specs_are_table_driven(self):
        rows = {s["rule"]: s for s in cmd_plan_check.VOLUME_SPECS}
        self.assertEqual(sorted(rows), ["R-PLAN-VOL03", "R-PLAN-VOL04", "R-PLAN-VOL05"])
        self.assertEqual((rows["R-PLAN-VOL04"]["first"], rows["R-PLAN-VOL04"]["last"]), (61, 80))
        self.assertEqual((rows["R-PLAN-VOL05"]["first"], rows["R-PLAN-VOL05"]["last"]), (81, 100))
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
        rec = self.record(VOL04, "SL-EP-062")
        other = self.work_order_not_tracing("SL-EP-062")
        self.assertNotIn(other, rec["work_orders"])
        self.edit_record("SL-EP-062", '"primary_wo": %s' % json.dumps(rec["primary_wo"]), '"primary_wo": "%s"' % other)
        self.assert_vol04("SL-EP-062 primary_wo %s is not in its work_orders" % other)

    def test_primary_must_be_one_id(self):
        rec = self.record(VOL04, "SL-EP-062")
        two = [rec["primary_wo"], self.tracing_non_primary(rec)]
        self.edit_record("SL-EP-062", '"primary_wo": %s' % json.dumps(rec["primary_wo"]),
                         '"primary_wo": %s' % json.dumps(two))
        self.assert_vol04("SL-EP-062 primary_wo must be exactly one work order id")

    def test_acceptance_absent_from_primary(self):
        rec = self.record(VOL04, "SL-EP-062")
        self.edit_record("SL-EP-062", '"acceptance": %s' % json.dumps(rec["acceptance"]),
                         '"acceptance": "just fabric-test codec"')
        self.assert_vol04("SL-EP-062 acceptance 'just fabric-test codec' is not in the front-matter acceptance",
                          "its primary_wo %s" % rec["primary_wo"])

    def test_acceptance_recipe_must_exist(self):
        rec = self.record(VOL04, "SL-EP-062")
        self.edit_record("SL-EP-062", '"acceptance": %s' % json.dumps(rec["acceptance"]),
                         '"acceptance": "just no-such-recipe"')
        self.assert_vol04("SL-EP-062 acceptance 'just no-such-recipe' is not `just <recipe>` for a recipe in the "
                          "justfile")

    def test_tracing_work_order_must_be_listed(self):
        rec = self.record(VOL04, "SL-EP-062")
        dropped = self.tracing_non_primary(rec)
        kept = [wo for wo in rec["work_orders"] if wo != dropped]
        self.edit_record("SL-EP-062", rec["wos_text"], '"work_orders": %s' % json.dumps(kept))
        self.assert_vol04("%s traces SL-EP-062 but the SL-EP-062 record does not list it" % dropped)

    def test_listed_work_order_must_trace(self):
        rec = self.record(VOL04, "SL-EP-062")
        extra = self.work_order_not_tracing("SL-EP-062")
        self.edit_record("SL-EP-062", rec["wos_text"], '"work_orders": %s' % json.dumps(rec["work_orders"] + [extra]))
        self.assert_vol04("SL-EP-062 lists %s but its Traces line does not name SL-EP-062" % extra)

    def test_unknown_work_order(self):
        rec = self.record(VOL04, "SL-EP-062")
        unknown = self.unknown_work_order()
        self.edit_record("SL-EP-062", rec["wos_text"], '"work_orders": %s' % json.dumps(rec["work_orders"] + [unknown]))
        self.assert_vol04("SL-EP-062 lists %s which has no file in work-orders/" % unknown)

    def test_a_work_order_is_primary_for_at_most_one_episode(self):
        rec61 = self.record(VOL04, "SL-EP-061")
        rec62 = self.record(VOL04, "SL-EP-062")
        moved = rec62["primary_wo"]
        self.assertNotIn(moved, rec61["work_orders"])
        cmds, err = cmd_plan_check.wo_acceptance(self.root, moved)
        self.assertIsNone(err)
        cmds = [c for c in cmds if c != rec62["acceptance"]] or cmds
        self.edit("work-orders/%s.md" % moved, "Traces: ", "Traces: SL-EP-061 ")
        self.edit_record(
            "SL-EP-061",
            rec61["head"],
            '"work_orders": %s, "primary_wo": "%s", "acceptance": %s'
            % (json.dumps(sorted(rec61["work_orders"] + [moved])), moved, json.dumps(cmds[0])),
        )
        self.assert_vol04("%s is primary_wo of more than one episode (SL-EP-061, SL-EP-062)" % moved)

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


class Vol05Test(Tree):
    """R-PLAN-VOL05 (WO-075): SL-VOL-05-SPEC gets the R-PLAN-VOL04 checks for episodes 081-100."""

    def test_row_mirrors_vol04(self):
        rows = {s["rule"]: s for s in cmd_plan_check.VOLUME_SPECS}
        vol04, vol05 = rows["R-PLAN-VOL04"], rows["R-PLAN-VOL05"]
        self.assertEqual(vol05["doc"], "SL-VOL-05-SPEC")
        self.assertEqual(vol05["keys"], vol04["keys"])
        self.assertEqual(vol05["named"], (81, 100, "WO-075 to WO-101"))
        self.assertTrue(vol05["exact_traces"] and vol05["primary"])
        self.assertIn("SL-VOL-05-SPEC", cmd_plan_check.EP_SCANNED)
        self.assertEqual(cmd_plan_check.DOCS["SL-VOL-05-SPEC"], VOL05)

    def test_missing_record(self):
        path = self.path(VOL05)
        with open(path, encoding="utf-8") as fh:
            lines = [ln for ln in fh.read().split("\n") if '"SL-EP-090"' not in ln]
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        self.assert_vol05("records must cover exactly SL-EP-081 to SL-EP-100", "SL-EP-090")

    def test_second_primary_for_wo090(self):
        """The planted case of the panel's plan-2: SL-EP-093's primary (WO-090) named primary of SL-EP-089 too."""
        rec89 = self.record(VOL05, "SL-EP-089")
        second = self.record(VOL05, "SL-EP-093")["primary_wo"]
        self.assertNotIn(second, rec89["work_orders"])
        self.edit_record("SL-EP-089", '"primary_wo": %s' % json.dumps(rec89["primary_wo"]),
                         '"primary_wo": "%s"' % second, VOL05)
        self.assert_vol05("SL-EP-089 primary_wo %s is not in its work_orders" % second)
        self.assert_vol05("%s is primary_wo of more than one episode (SL-EP-089, SL-EP-093)" % second)

    def test_acceptance_absent_from_primary(self):
        rec = self.record(VOL05, "SL-EP-089")
        self.edit_record("SL-EP-089", '"acceptance": %s' % json.dumps(rec["acceptance"]),
                         '"acceptance": "just runtime-test frame"', VOL05)
        self.assert_vol05("SL-EP-089 acceptance 'just runtime-test frame' is not in the front-matter acceptance",
                          "its primary_wo %s" % rec["primary_wo"])

    def test_tracing_work_order_must_be_listed(self):
        rec = self.record(VOL05, "SL-EP-082")
        dropped = self.tracing_non_primary(rec)
        kept = [wo for wo in rec["work_orders"] if wo != dropped]
        self.edit_record("SL-EP-082", rec["wos_text"], '"work_orders": %s' % json.dumps(kept), VOL05)
        self.assert_vol05("%s traces SL-EP-082 but the SL-EP-082 record does not list it" % dropped)

    def test_title_must_equal_registry(self):
        self.edit(VOL05, '"title": "Frame scheduler"', '"title": "Scheduler"')
        self.assert_vol05("SL-EP-090 title", "differs from the registry")


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
