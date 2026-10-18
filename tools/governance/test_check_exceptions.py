"""Unit tests for exception-check (cmd_check_exceptions.py, WO-009)."""

import copy
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_exceptions as cx  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

SCHEMA = slib.load_schema("sl-reg-006", os.path.join(slgov.ROOT, "schemas"))


def good():
    return {
        "schema": "sl-reg-006/1",
        "classes": [
            {"id": "SL-EXC-C%d" % (i + 1), "name": name, "description": "d"} for i, name in enumerate(cx.CLASSES)
        ],
        "exceptions": [],
    }


def exc(**over):
    base = {
        "id": "SL-EXC-001",
        "class": "SL-EXC-C2",
        "subject": "F8 store signing step",
        "reason": "The platform owner signs the package",
        "episodes": ["SL-EP-420"],
        "approver": "founder",
        "approved": "2026-10-17",
        "expires": "2027-01-01",
        "adr": None,
    }
    base.update(over)
    return base


def rules(errors):
    return {m.group(0) for e in errors for m in [re.search(r"R-EXC-[A-Z-]+", e)] if m}


def check(register, docs=None):
    return cx.register_errors(register, SCHEMA, docs)


class ExceptionCheckTests(unittest.TestCase):
    def test_real_register_passes(self):
        self.assertEqual(cx.run(slgov.ROOT), [])

    def test_good_register_with_zero_exceptions(self):
        self.assertEqual(check(good(), list(cx.CLASSES)), [])

    def test_good_exception_passes(self):
        reg = good()
        reg["exceptions"].append(exc())
        self.assertEqual(check(reg), [])

    def test_missing_or_extra_class(self):
        reg = good()
        del reg["classes"][5]
        self.assertEqual(rules(check(reg)), {"R-EXC-CLASSES"})
        reg = good()
        reg["classes"].append({"id": "SL-EXC-C7", "name": "other", "description": "d"})
        self.assertEqual(rules(check(reg)), {"R-EXC-CLASSES"})

    def test_renamed_class(self):
        reg = good()
        reg["classes"][0]["name"] = "console certificate"
        self.assertEqual(rules(check(reg)), {"R-EXC-CLASSES"})

    def test_document_list_differs(self):
        self.assertEqual(rules(check(good(), list(cx.CLASSES[:5]))), {"R-EXC-CLASSES"})

    def test_duplicate_ids(self):
        reg = good()
        reg["classes"][1]["id"] = "SL-EXC-C1"
        self.assertIn("R-EXC-DUP-ID", rules(check(reg)))
        reg = good()
        reg["exceptions"] = [exc(), exc()]
        self.assertIn("R-EXC-DUP-ID", rules(check(reg)))

    def test_schema_violation(self):
        reg = good()
        del reg["exceptions"]
        self.assertIn("R-EXC-SCHEMA", rules(check(reg)))
        reg = good()
        reg["exceptions"].append(exc(extra=1))
        self.assertIn("R-EXC-SCHEMA", rules(check(reg)))

    def test_unknown_class(self):
        reg = good()
        reg["exceptions"].append(exc(**{"class": "SL-EXC-C9"}))
        self.assertEqual(rules(check(reg)), {"R-EXC-CLASS"})

    def test_invariant(self):
        for subject in ("no invariant named", "F9 out of range", "F1 and F2 both", "xF3"):
            reg = good()
            reg["exceptions"].append(exc(subject=subject))
            self.assertEqual(rules(check(reg)), {"R-EXC-INVARIANT"}, subject)

    def test_blank_reason_and_owner(self):
        reg = good()
        reg["exceptions"].append(exc(reason="  ", approver=" "))
        self.assertEqual(rules(check(reg)), {"R-EXC-REASON", "R-EXC-OWNER"})

    def test_expires_must_be_a_real_date(self):
        for value in (None, "2027-13-40", "2027-02-30"):
            reg = good()
            reg["exceptions"].append(exc(expires=value))
            self.assertIn("R-EXC-EXPIRES", rules(check(reg)), value)

    def test_review_dates(self):
        reg = good()
        reg["exceptions"].append(exc(approved="2028-01-01"))
        self.assertEqual(rules(check(reg)), {"R-EXC-REVIEW"})
        reg = good()
        reg["exceptions"].append(exc(approved="2026-02-30"))
        self.assertIn("R-EXC-REVIEW", rules(check(reg)))

    def test_input_errors(self):
        self.assertEqual(rules(cx.run(os.path.join(slgov.ROOT, "no-such-root"))), {"R-EXC-INPUT"})

    def test_good_is_not_mutated(self):
        reg = good()
        before = copy.deepcopy(reg)
        check(reg)
        self.assertEqual(reg, before)


if __name__ == "__main__":
    unittest.main()
