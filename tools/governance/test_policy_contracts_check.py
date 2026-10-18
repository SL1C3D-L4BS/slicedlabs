"""Unit tests for policy-contracts-check and gov008-lock-check."""

import copy
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_gov008_lock_check  # noqa: E402
import cmd_policy_contracts_check as pcc  # noqa: E402
import slgov  # noqa: E402

RECIPES = {"gov008-lock-check": {}, "deps-check": {}, "neg-deps": {}}


def good_policy():
    return {
        "contracts": [
            {"id": "C%02d" % i, "text": "t", "enforced_by": "just deps-check" if i % 2 else "pending", "episodes": [48]}
            for i in range(1, 13)
        ],
        "providers": ["vendored", "system", "cpm", "vcpkg"],
        "provider_order": {
            "ci": ["vendored", "cpm", "system", "vcpkg"],
            "dev": ["system", "vendored", "cpm", "vcpkg"],
        },
        "tiers": {"A": "content-addressed", "B": "range-pinned"},
        "licenses_allowed": ["MIT", "Zlib"],
    }


def doc(policy):
    return "# T\n\n```sl-data\n%s\n```\n" % json.dumps(policy, indent=1)


def rules(errors):
    return {e.split(": ")[1] for e in errors}


class PolicyTests(unittest.TestCase):
    def test_good_policy_passes(self):
        self.assertEqual(pcc.policy_errors(doc(good_policy()), RECIPES), [])

    def test_real_document_passes(self):
        self.assertEqual(pcc.run(slgov.ROOT), [])

    def test_no_block_or_two_blocks(self):
        self.assertEqual(rules(pcc.policy_errors("# T\n", RECIPES)), {"R-POL-BLOCK"})
        self.assertEqual(rules(pcc.policy_errors(doc(good_policy()) * 2, RECIPES)), {"R-POL-BLOCK"})

    def test_bad_json(self):
        self.assertEqual(rules(pcc.policy_errors("```sl-data\n{nope\n```\n", RECIPES)), {"R-POL-BLOCK"})

    def test_missing_contract(self):
        p = good_policy()
        del p["contracts"][5]
        self.assertIn("R-POL-CONTRACTS", rules(pcc.policy_errors(doc(p), RECIPES)))

    def test_duplicate_contract(self):
        p = good_policy()
        p["contracts"][3]["id"] = "C03"
        self.assertIn("R-POL-CONTRACTS", rules(pcc.policy_errors(doc(p), RECIPES)))

    def test_unknown_recipe(self):
        p = good_policy()
        p["contracts"][0]["enforced_by"] = "just no-such-recipe"
        self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-ENFORCER"})

    def test_enforcer_must_be_recipe_or_pending(self):
        for bad in ("", "deps-check", "todo", None):
            p = good_policy()
            p["contracts"][0]["enforced_by"] = bad
            with self.subTest(bad):
                self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-ENFORCER"})

    def test_provider_order_must_be_permutation(self):
        p = good_policy()
        p["provider_order"]["ci"] = ["vendored", "cpm", "system"]
        self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-PROVIDERS"})
        p = good_policy()
        p["provider_order"]["dev"] = ["system", "system", "cpm", "vcpkg"]
        self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-PROVIDERS"})

    def test_empty_licenses(self):
        p = good_policy()
        p["licenses_allowed"] = []
        self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-LICENSES"})

    def test_tiers_required(self):
        p = good_policy()
        del p["tiers"]["B"]
        self.assertEqual(rules(pcc.policy_errors(doc(p), RECIPES)), {"R-POL-TIERS"})


class LockTests(unittest.TestCase):
    def tree(self, lock):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        gov = os.path.join(d.name, "docs", "gov")
        os.makedirs(gov)
        with open(os.path.join(d.name, *pcc.DOC_REL.split("/")), "w", encoding="utf-8") as fh:
            fh.write(doc(good_policy()))
        if lock is not None:
            os.makedirs(os.path.join(d.name, "deps"))
            with open(os.path.join(d.name, "deps", "deps.lock.json"), "w", encoding="utf-8") as fh:
                fh.write(lock if isinstance(lock, str) else json.dumps(lock))
        return d.name

    def lock_for(self):
        p = good_policy()
        return {"policy": {k: p[k] for k in cmd_gov008_lock_check.KEYS}}

    def test_absent_lock_passes(self):
        errors, present = cmd_gov008_lock_check.run(self.tree(None))
        self.assertEqual((errors, present), ([], False))

    def test_matching_lock_passes(self):
        errors, present = cmd_gov008_lock_check.run(self.tree(self.lock_for()))
        self.assertEqual((errors, present), ([], True))

    def test_licence_order_is_irrelevant(self):
        lock = self.lock_for()
        lock["policy"]["licenses_allowed"].reverse()
        self.assertEqual(cmd_gov008_lock_check.run(self.tree(lock))[0], [])

    def test_drift_is_reported_per_key(self):
        lock = self.lock_for()
        lock["policy"]["provider_order"] = copy.deepcopy(lock["policy"]["provider_order"])
        lock["policy"]["provider_order"]["ci"].reverse()
        lock["policy"]["licenses_allowed"].append("GPL-3.0-only")
        errors, _ = cmd_gov008_lock_check.run(self.tree(lock))
        self.assertEqual(rules(errors), {"R-LOCK-DRIFT"})
        self.assertEqual(len(errors), 2)

    def test_missing_policy_object_and_bad_json(self):
        self.assertEqual(rules(cmd_gov008_lock_check.run(self.tree({"schema": "x"}))[0]), {"R-LOCK-READ"})
        self.assertEqual(rules(cmd_gov008_lock_check.run(self.tree("{nope"))[0]), {"R-LOCK-READ"})


if __name__ == "__main__":
    unittest.main()
