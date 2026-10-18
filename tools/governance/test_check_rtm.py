"""Unit tests for rtm-check (cmd_check_rtm.py)."""

import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_rtm  # noqa: E402
import slgov  # noqa: E402

FIXTURES = os.path.join(slgov.ROOT, "tests", "governance", "fixtures", "rtm")
GOOD = os.path.join(FIXTURES, "good")
DOC_IDS = {"SL-ARCH-001", "SL-GOV-006"}
EPISODES = {"SL-EP-001", "SL-EP-002"}
RECIPES = {"doc-check"}


def load(rel):
    with open(os.path.join(GOOD, rel), encoding="utf-8") as fh:
        return json.load(fh)


def catalogue():
    with open(os.path.join(GOOD, "docs", "gov", "SL-GOV-002-requirements-catalogue.md"), encoding="utf-8") as fh:
        rows, errors = cmd_check_rtm.catalogue_requirements(fh.read(), "doc")
    assert not errors, errors
    return rows


def run(register, cat=None):
    return cmd_check_rtm.rtm_errors(cat if cat is not None else catalogue(), register, DOC_IDS, EPISODES, RECIPES)


def rules(errors):
    return {e.split(": ")[1] for e in errors}


def good_register():
    return load("registry/SL-REG-003-traceability.json")


class RtmTests(unittest.TestCase):
    def test_good_tree_passes(self):
        self.assertEqual(run(good_register()), [])
        self.assertEqual(cmd_check_rtm.check(GOOD), [])

    def test_each_bad_register_fixture_names_its_rule(self):
        names = sorted(n for n in os.listdir(FIXTURES) if n.startswith("bad-") and n.endswith(".json"))
        self.assertGreaterEqual(len(names), 10)
        for name in names:
            expected = name[len("bad-") : -len(".json")].upper()
            with open(os.path.join(FIXTURES, name), encoding="utf-8") as fh:
                register = json.load(fh)
            with self.subTest(name):
                self.assertIn(expected, rules(run(register)))

    def test_bad_catalogue_fixture_is_a_block_error(self):
        with open(os.path.join(FIXTURES, "bad-r-rtm-block.md"), encoding="utf-8") as fh:
            _, errors = cmd_check_rtm.catalogue_requirements(fh.read(), "doc")
        self.assertIn("R-RTM-BLOCK", rules(errors))

    def test_dangling_deferred_episode(self):
        reg = good_register()
        reg["requirements"][0]["tests"] = ["deferred:SL-EP-777"]
        self.assertIn("R-RTM-TEST", rules(run(reg)))

    def test_test_forms(self):
        for bad in ("make check", "just", "deferred:001", "just doc-check now"):
            reg = good_register()
            reg["requirements"][0]["tests"] = [bad]
            with self.subTest(bad):
                self.assertIn("R-RTM-TEST", rules(run(reg)))

    def test_empty_lists_fail(self):
        for key, rule in (("architecture", "R-RTM-ARCH"), ("episodes", "R-RTM-EPISODE"), ("tests", "R-RTM-TEST")):
            reg = good_register()
            reg["requirements"][0][key] = []
            with self.subTest(key):
                self.assertIn(rule, rules(run(reg)))

    def test_measure_must_be_complete(self):
        reg = good_register()
        del reg["requirements"][0]["measure"]["den"]
        self.assertIn("R-RTM-EVIDENCE", rules(run(reg)))
        reg = good_register()
        del reg["requirements"][0]["measure"]
        self.assertIn("R-RTM-EVIDENCE", rules(run(reg)))

    def test_duplicate_register_id(self):
        reg = good_register()
        reg["requirements"].append(copy.deepcopy(reg["requirements"][0]))
        self.assertIn("R-RTM-ID", rules(run(reg)))

    def test_catalogue_drift_and_orphan(self):
        cat = catalogue()
        cat[0]["src"] = "master plan section 9"
        self.assertIn("R-RTM-DRIFT", rules(run(good_register(), cat)))
        self.assertIn("R-RTM-ORPHAN", rules(run(good_register(), cat[:1])))

    def test_register_shape(self):
        self.assertIn("R-RTM-SHAPE", rules(run({"schema": "other", "requirements": []})))
        self.assertIn("R-RTM-SHAPE", rules(run({"schema": "sl-reg-003/1", "requirements": ["x"]})))

    def test_missing_catalogue_in_tree(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("R-RTM-BLOCK", rules(cmd_check_rtm.check(tmp)))

    def test_real_repository_is_clean(self):
        self.assertEqual(cmd_check_rtm.check(slgov.ROOT), [])


if __name__ == "__main__":
    unittest.main()
