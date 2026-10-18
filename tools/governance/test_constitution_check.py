"""Unit tests for constitution-check and contracts-pending."""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check as cc  # noqa: E402
import cmd_contracts_pending as cp  # noqa: E402
import slgov  # noqa: E402


def read(rel):
    with open(os.path.join(slgov.ROOT, *rel.split("/")), encoding="utf-8") as fh:
        return fh.read()


PLAN = read(cc.PLAN_REL)
CONSTITUTION = read(cc.CONSTITUTION_REL)
BUDGET = read(cc.BUDGET_REL)


class PlanParseTests(unittest.TestCase):
    def test_locked_table_has_19_rows(self):
        rows = cc.plan_locked(PLAN)
        self.assertEqual(len(rows), 19)
        self.assertEqual(rows[1], ("Build system", "CMake + Ninja for native code; Sliced build graph for content and cooking"))
        self.assertIn("(Vol 31)", rows[8][1])

    def test_fragmentation_table_is_f1_to_f8(self):
        rows = cc.plan_fragmentation(PLAN)
        self.assertEqual([r["id"] for r in rows], ["F%d" % n for n in range(1, 9)])
        self.assertEqual(rows[1]["numerator"], "Data domains with >1 registered owner")
        self.assertEqual(rows[0]["episodes"], ["SL-EP-003", "SL-EP-200", "SL-EP-220", "SL-EP-408"])
        self.assertEqual(rows[6]["threshold"], "≤ 1.05 (baseline set in ep. 198)")

    def test_exemptions_and_scorecard(self):
        self.assertEqual(len(cc.plan_exemptions(PLAN)), 6)
        self.assertEqual(cc.plan_exemptions(PLAN)[-1], "AI model training")
        self.assertEqual(len(cc.plan_scorecard(PLAN)), 7)


class RealDocumentTests(unittest.TestCase):
    def test_real_documents_pass(self):
        self.assertEqual(cc.check_all(slgov.ROOT), [])


class MutationTests(unittest.TestCase):
    def test_changed_decision_text_fails_locked(self):
        text = CONSTITUTION.replace("QUIC for services", "TCP for services")
        errors = cc.locked_errors(text, PLAN)
        self.assertTrue(errors and all("R-CONST-LOCKED" in e for e in errors))

    def test_dropped_row_fails_locked(self):
        text = CONSTITUTION.replace('{"area": "Audio", "decision": "Native mixer and DSP graph; native procedural audio graph"},\n', "")
        self.assertNotEqual(text, CONSTITUTION)
        errors = cc.locked_errors(text, PLAN)
        self.assertTrue(any("18 locked rows" in e for e in errors))
        self.assertTrue(any("'Audio' is missing" in e for e in errors))

    def test_rejected_item_must_appear_in_its_row(self):
        text = CONSTITUTION.replace('"item": "arbitrary C++ reload"', '"item": "hot patching"')
        self.assertTrue(any("does not appear" in e for e in cc.locked_errors(text, PLAN)))

    def test_whitespace_is_normalised(self):
        text = CONSTITUTION.replace("Archetype ECS; job system", "Archetype  ECS;   job system")
        self.assertEqual(cc.locked_errors(text, PLAN), [])

    def test_changed_threshold_fails_frag(self):
        text = BUDGET.replace('"threshold": "0", "enforcement": "Currency check', '"threshold": "1", "enforcement": "Currency check')
        self.assertNotEqual(text, BUDGET)
        errors = cc.frag_errors(text, PLAN)
        self.assertTrue(errors and all("R-CONST-FRAG" in e for e in errors))
        self.assertTrue(any("F4.threshold" in e for e in errors))

    def test_changed_enforcement_and_missing_row_fail_frag(self):
        text = BUDGET.replace("Trace schema validator", "Manual review")
        self.assertTrue(any("F6.enforcement" in e for e in cc.frag_errors(text, PLAN)))
        text = BUDGET.replace('{"id": "F8"', '{"id": "F9"')
        errors = cc.frag_errors(text, PLAN)
        self.assertTrue(any("F8 is missing" in e for e in errors))

    def test_exemption_class_drift_fails_frag(self):
        text = BUDGET.replace('"platform signing", ', "")
        self.assertTrue(any("exemptions" in e for e in cc.frag_errors(text, PLAN)))

    def test_missing_block_and_bad_json(self):
        self.assertTrue(any("no sl-data block" in e for e in cc.locked_errors("# nothing\n", PLAN)))
        self.assertTrue(any("not JSON" in e for e in cc.frag_errors("```sl-data\n{bad\n```\n", PLAN)))


class ContractsPendingTests(unittest.TestCase):
    STATE = {"doc-check": (False, "WO-003"), "rtm-check": (True, "WO-007")}

    def test_classify(self):
        self.assertEqual(cp.classify("pending", self.STATE), "pending")
        self.assertEqual(cp.classify("pending (episode 21)", self.STATE), "pending")
        self.assertIsNone(cp.classify("just doc-check", self.STATE))
        self.assertIsNone(cp.classify("Trace schema validator", self.STATE))
        self.assertIn("stub (owner WO-007)", cp.classify("just rtm-check", self.STATE))
        self.assertIn("stub", cp.classify("rtm-check", self.STATE))
        self.assertIn("unknown recipe nope", cp.classify("just nope", self.STATE))
        self.assertIn("pending", cp.classify(["just doc-check", "pending"], self.STATE))

    def test_real_repo_lists_the_f_rows_and_exits_zero(self):
        found, warnings = cp.pending_contracts(slgov.ROOT)
        self.assertEqual(warnings, [])
        self.assertEqual(sum("SL-GOV-006" in loc for loc, _ in found), 8)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cp.main(["cmd_contracts_pending.py"]), 0)

    def test_never_fails_on_broken_documents(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "docs", "gov"))
            with open(os.path.join(root, "docs", "gov", "SL-GOV-900-x.md"), "w", encoding="utf-8") as fh:
                fh.write('```sl-data\n{broken\n```\n```sl-data\n{"c": [{"id": "C1", "verified_by": "just rtm-check"}]}\n```\n')
            found, warnings = cp.pending_contracts(root)
            self.assertEqual(len(warnings), 1)
            self.assertEqual(len(found), 1)  # no justfile in the temp root: the recipe is unknown
            self.assertIn("unknown recipe rtm-check", found[0][1])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cp.main(["x", "--root", root]), 0)


if __name__ == "__main__":
    unittest.main()
