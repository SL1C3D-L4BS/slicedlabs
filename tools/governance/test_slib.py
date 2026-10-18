"""Unit tests for slib: JSON loading, the schema subset validator, the lifecycle FSM, the rule engine."""

import copy
import glob
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slib  # noqa: E402

FIXTURES = os.path.join(slib.ROOT, "tests", "governance", "fixtures")


def good():
    return slib.load_json(os.path.join(FIXTURES, "good-minimal.json"))


def rules_of(bundle):
    return {f["rule"] for f in slib.check_bundle(bundle)}


class LoadJsonTests(unittest.TestCase):
    def test_duplicate_keys_are_rejected(self):
        with self.assertRaises(slib.LoadError):
            slib.loads_json('{"a": 1, "a": 2}')

    def test_malformed_and_missing_files(self):
        with self.assertRaises(slib.LoadError):
            slib.loads_json("{")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(slib.LoadError):
                slib.load_json(os.path.join(tmp, "absent.json"))


class ValidatorTests(unittest.TestCase):
    def test_every_keyword_of_the_subset(self):
        schema = {
            "type": "object",
            "required": ["id"],
            "properties": {
                "id": {"type": "string", "pattern": "^A-[0-9]+$", "minLength": 3},
                "n": {"type": "integer", "minimum": 1, "maximum": 3},
                "kind": {"enum": ["x", "y"]},
                "tags": {"type": "array", "items": {"type": "string"}, "uniqueItems": True, "minItems": 1},
                "v": {"const": 1},
                "r": {"$ref": "#/$defs/s"},
            },
            "additionalProperties": False,
            "$defs": {"s": {"type": ["string", "null"]}},
        }
        ok = {"id": "A-1", "n": 2, "kind": "x", "tags": ["a"], "v": 1, "r": None}
        self.assertEqual(slib.validate(ok, schema), [])
        for bad in (
            {"n": 2},
            dict(ok, id="B-1"),
            dict(ok, n=0),
            dict(ok, n=4),
            dict(ok, kind="z"),
            dict(ok, tags=[]),
            dict(ok, tags=["a", "a"]),
            dict(ok, v=True),
            dict(ok, r=5),
            dict(ok, extra=1),
        ):
            self.assertTrue(slib.validate(bad, schema), bad)

    def test_error_paths_are_json_pointers(self):
        schema = {"properties": {"a": {"items": {"type": "integer"}}}}
        errors = slib.validate({"a": [1, "x"]}, schema)
        self.assertEqual([p for p, _ in errors], ["/a/1"])

    def test_bool_is_not_an_integer_and_one_equals_one_point_zero(self):
        self.assertFalse(slib.is_valid(True, {"type": "integer"}))
        self.assertTrue(slib.is_valid(1.0, {"type": "integer"}))
        self.assertFalse(slib.is_valid(True, {"enum": [1]}))
        self.assertTrue(slib.is_valid([1, True], {"uniqueItems": True}))

    def test_unsupported_keywords_are_refused(self):
        self.assertTrue(slib.check_schema_subset({"anyOf": []}))
        self.assertTrue(slib.check_schema_subset({"properties": {"a": {"format": "date"}}}))
        self.assertTrue(slib.check_schema_subset({"$ref": "other.json#/x"}))
        with self.assertRaises(slib.SchemaError):
            slib.validate(1, {"oneOf": []})

    def test_unresolvable_ref_is_an_error(self):
        with self.assertRaises(slib.SchemaError):
            slib.validate(1, {"$ref": "#/$defs/missing"})

    def test_shipped_schemas_stay_in_the_subset(self):
        paths = glob.glob(os.path.join(slib.SCHEMA_DIR, "sl-*.schema.json"))
        self.assertGreaterEqual(len(paths), 8)
        for path in paths:
            schema = slib.load_json(path)
            self.assertEqual(slib.check_schema_subset(schema), [], path)
            self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")


class LifecycleTests(unittest.TestCase):
    # Master plan 6.2, edge by edge.
    PLAN_6_2 = {
        ("Planned", "Specified"),
        ("Specified", "InProgress"),
        ("InProgress", "Verifying"),
        ("Verifying", "InProgress"),
        ("Verifying", "Demonstrated"),
        ("Demonstrated", "Accepted"),
        ("Accepted", "Reopened"),
        ("Reopened", "InProgress"),
        ("Verifying", "Blocked"),
        ("Blocked", "InProgress"),
    }

    def test_edges_equal_master_plan_6_2(self):
        self.assertEqual(set(slib.fsm_edges()), self.PLAN_6_2)
        self.assertEqual(len(slib.fsm_edges()), len(self.PLAN_6_2))

    def test_shortcuts_are_illegal(self):
        self.assertFalse(slib.can_transition("Planned", "InProgress"))
        self.assertFalse(slib.can_transition("InProgress", "Demonstrated"))
        self.assertFalse(slib.can_transition("Demonstrated", "InProgress"))
        self.assertFalse(slib.can_transition("Accepted", "Planned"))
        self.assertFalse(slib.can_transition("Accepted", "Accepted"))
        self.assertTrue(slib.can_transition("Verifying", "Blocked"))
        self.assertEqual(slib.next_states("Verifying"), ["InProgress", "Demonstrated", "Blocked"])

    def test_every_state_is_reachable_from_planned(self):
        seen, todo = set(), ["Planned"]
        while todo:
            s = todo.pop()
            if s not in seen:
                seen.add(s)
                todo.extend(slib.next_states(s))
        self.assertEqual(seen, set(slib.STATES))


class RuleEngineTests(unittest.TestCase):
    def test_good_fixture_is_clean(self):
        self.assertEqual(slib.check_bundle(good()), [])

    def test_every_rule_has_a_fixture_failing_with_exactly_that_id(self):
        self.assertEqual(len(slib.RULES), 13)
        for rule in slib.RULES:
            path = os.path.join(FIXTURES, "bad-%s.json" % rule)
            self.assertTrue(os.path.isfile(path), path)
            findings = slib.check_bundle(slib.load_json(path))
            self.assertTrue(findings, rule)
            self.assertEqual({f["rule"] for f in findings}, {rule}, rule)
            for f in findings:
                self.assertEqual(set(f), {"rule", "path", "message"})
                self.assertTrue(f["path"].startswith("/"))

    def test_schema_findings_stop_the_other_rules(self):
        b = good()
        b["registry"]["episodes"][2]["status"] = "Done"
        b["registry"]["episodes"][3]["depends_on"] = ["SL-EP-005"]
        self.assertEqual(rules_of(b), {"R-SCHEMA"})

    def test_context_is_optional(self):
        b = good()
        for key in ("documents", "work_orders", "plan_sha256", "views"):
            del b[key]
        self.assertEqual(slib.check_bundle(b), [])
        b["registry"]["episodes"][2]["documents"] = ["SL-ARCH-009"]
        self.assertEqual(slib.check_bundle(b), [])

    def test_findings_are_sorted_by_rule_order(self):
        b = good()
        b["plan_sha256"] = slib.sha256_text("other")
        b["registry"]["episodes"][2]["documents"] = ["SL-ARCH-009"]
        order = [f["rule"] for f in slib.check_bundle(b)]
        self.assertEqual(order, ["R-DOC-MISSING", "R-PLAN-SHA"])

    def test_gap_and_dup_are_distinguished(self):
        b = good()
        b["registry"]["episodes"][3]["number"] = 5
        b["registry"]["episodes"][3]["id"] = "SL-EP-005"
        self.assertIn("R-DUP-ID", rules_of(b))

    def test_volume_self_unknown_and_cycle(self):
        for deps, expect in ((["SL-VOL-01"], "depends on itself"), (["SL-VOL-09"], "unknown volume"), (["SL-VOL-02"], "cycle")):
            b = good()
            b["registry"]["volumes"][0]["depends_on_volumes"] = deps
            msgs = [f["message"] for f in slib.check_bundle(b)]
            self.assertTrue(any(expect in m for m in msgs), (deps, msgs))
            self.assertEqual(rules_of(b), {"R-VOLGRAPH"})

    def test_forward_self_and_unknown_dependencies(self):
        for dep, expect in (("SL-EP-004", "later episode"), ("SL-EP-099", "unknown"), ("SL-EP-005", "itself")):
            b = good()
            b["registry"]["episodes"][4]["depends_on"] = [dep] if dep != "SL-EP-004" else ["SL-EP-005"]
            if dep == "SL-EP-004":
                b["registry"]["episodes"][3]["depends_on"] = ["SL-EP-005"]
                b["registry"]["episodes"][4]["depends_on"] = []
            msgs = [f["message"] for f in slib.check_bundle(b)]
            self.assertTrue(any(expect in m for m in msgs), (dep, msgs))

    def test_lifecycle_catches_a_skipped_state_and_a_wrong_final_status(self):
        b = good()
        b["registry"]["episodes"][2]["status"] = "Accepted"
        self.assertIn("R-LIFECYCLE", rules_of(b))
        b = good()
        b["registry"]["lifecycle"]["transitions"].append(["Planned", "InProgress"])
        self.assertEqual(rules_of(b), {"R-LIFECYCLE"})
        b = good()
        b["registry"]["episodes"][3]["status"] = "Specified"
        self.assertEqual(rules_of(b), {"R-LIFECYCLE"})

    def test_blocked_and_reopened_paths_are_legal(self):
        b = good()
        ep = b["registry"]["episodes"][2]
        d = "2026-10-17"
        ep["history"] = [
            {"from": "Planned", "to": "Specified", "date": d},
            {"from": "Specified", "to": "InProgress", "date": d},
            {"from": "InProgress", "to": "Verifying", "date": d},
            {"from": "Verifying", "to": "Blocked", "date": d},
            {"from": "Blocked", "to": "InProgress", "date": d},
        ]
        ep["status"] = "InProgress"
        self.assertEqual(slib.check_bundle(b), [])

    def test_dependency_acceptance_and_the_adr_waiver(self):
        b = good()
        b["registry"]["episodes"][2]["depends_on"] = ["SL-EP-002"]
        self.assertEqual(rules_of(b), {"R-DEP-ACCEPTED"})
        b["registry"]["lifecycle"]["waivers"] = [
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0003", "first": 3, "last": 3, "max_status": "Demonstrated"}
        ]
        self.assertEqual(slib.check_bundle(b), [])
        # the waiver stops at its ceiling: Accepted still needs accepted dependencies
        ep = b["registry"]["episodes"][2]
        ep["status"] = "Accepted"
        ep["history"] = [
            {"from": "Planned", "to": "Specified", "date": "2026-10-17"},
            {"from": "Specified", "to": "InProgress", "date": "2026-10-17"},
            {"from": "InProgress", "to": "Verifying", "date": "2026-10-17"},
            {"from": "Verifying", "to": "Demonstrated", "date": "2026-10-17"},
            {"from": "Demonstrated", "to": "Accepted", "date": "2026-10-17"},
        ]
        ep["completion"] = {"revision": "r", "evidence": ["evidence/WO-003.json"], "benchmarks": [], "open_defects": [], "exceptions": []}
        self.assertEqual(rules_of(b), {"R-DEP-ACCEPTED"})

    def test_evidence_needs_a_completion_record(self):
        b = good()
        b["registry"]["episodes"][0]["completion"] = None
        self.assertEqual(rules_of(b), {"R-NO-EVIDENCE"})

    def test_amendments_make_a_scope_change_visible(self):
        b = good()
        ep = b["registry"]["episodes"][2]
        old = ep["meaning_sha256"]
        ep["title"] = "Linear algebra, rewritten"
        ep["meaning_sha256"] = slib.meaning_sha256(ep["title"], ep["summary"])
        self.assertEqual(slib.check_bundle(b), [])  # hash updated, no amendment: the lock guards this
        ep["amendments"] = [
            {"date": "2026-10-17", "note": "n", "impact": "i", "adr": None, "from_sha256": old,
             "to_sha256": ep["meaning_sha256"], "traceability_updated": False}
        ]
        self.assertEqual(rules_of(b), {"R-SILENT-SCOPE"})
        ep["amendments"][0]["traceability_updated"] = True
        self.assertEqual(slib.check_bundle(b), [])
        ep["decision_status"] = "Locked"
        self.assertEqual(rules_of(b), {"R-SILENT-SCOPE"})
        ep["amendments"][0]["adr"] = "ADR-0004"
        self.assertEqual(slib.check_bundle(b), [])

    def test_work_order_trace_directions(self):
        b = good()
        b["work_orders"][0]["traces"] = ["SL-EP-002"]
        msgs = [f["message"] for f in slib.check_bundle(b)]
        self.assertEqual(rules_of(b), {"R-WO-TRACE"})
        self.assertTrue(any("does not trace" in m for m in msgs))
        b = good()
        b["work_orders"].pop()
        self.assertEqual(rules_of(b), {"R-WO-TRACE"})
        b = good()
        b["work_orders"][0]["traces"] = []
        self.assertEqual(rules_of(b), {"R-SCHEMA"})

    def test_summary_may_be_null_and_hashes_differ_by_summary(self):
        self.assertNotEqual(slib.meaning_sha256("T", None), slib.meaning_sha256("T", "S"))
        self.assertEqual(slib.meaning_sha256("T", None), slib.sha256_text("T\n"))

    def test_input_is_not_mutated(self):
        b = good()
        before = copy.deepcopy(b)
        slib.check_bundle(b)
        self.assertEqual(b, before)

    def test_bundle_shape_errors(self):
        self.assertEqual({f["rule"] for f in slib.check_bundle({})}, {"R-SCHEMA"})
        b = good()
        b["surprise"] = 1
        self.assertEqual(rules_of(b), {"R-SCHEMA"})


if __name__ == "__main__":
    unittest.main()
