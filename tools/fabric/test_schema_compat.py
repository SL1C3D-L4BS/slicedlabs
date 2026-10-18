"""Unit tests of the schema compatibility classifier (WO-040). FAB-INV-SCH-01: the golden table."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema_compat  # noqa: E402
import schemalib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(ROOT, "tests", "fixtures", "fabric", "schema", "compat_table.json")


def load_table():
    with open(TABLE, encoding="utf-8") as fh:
        return json.load(fh)["rows"]


class GoldenTable(unittest.TestCase):
    def setUp(self):
        arch, errors = schemalib.load_arch(ROOT)
        self.assertEqual(errors, [])
        self.arch_rows = {r["change"]: r["class"] for r in arch["compatibility"]}
        self.rows = load_table()

    def test_fab_inv_sch_01_golden_table(self):
        # the classifier table equals the SL-ARCH-001 `compatibility` table, row for row
        self.assertEqual({k: v[1] for k, v in schema_compat.ROWS.items()}, self.arch_rows)
        self.assertEqual(len(self.arch_rows), 23)
        # every ARCH row has at least one golden row, and every golden row names an ARCH row
        self.assertEqual({r["change"] for r in self.rows}, set(self.arch_rows))
        self.assertEqual(len({r["case"] for r in self.rows}), len(self.rows))
        for row in self.rows:
            with self.subTest(case=row["case"]):
                self.assertEqual(row["expected"], self.arch_rows[row["change"]])
                self.assertEqual(schema_compat.classify(row["old"], row["new"]), row["expected"])
                found = [c.code for c in schema_compat.changes(row["old"], row["new"])]
                if row["case"] != "unchanged":
                    code = schema_compat.ROWS[row["change"]][0]
                    extra = {"change_state_class", "change_record_class"} if code == "change_domain" else set()
                    self.assertTrue(code in found or set(found) & extra, found)

    def test_unchanged_is_identical_and_has_no_changes(self):
        for row in self.rows:
            self.assertEqual(schema_compat.changes(row["old"], row["old"]), [])
            if row["expected"] != "forbidden":  # a reserved number that is also a field is not a valid type
                self.assertEqual(schema_compat.classify(row["new"], row["new"]), "identical")

    def test_required_bump_follows_the_arch_table(self):
        self.assertEqual(schema_compat.required_bump("identical"), ("none", "none"))
        self.assertEqual(schema_compat.required_bump("additive"), ("minor", "none"))
        self.assertEqual(schema_compat.required_bump("backward_only"), ("major", "identity"))
        self.assertEqual(schema_compat.required_bump("breaking"), ("major", "step"))
        self.assertEqual(schema_compat.required_bump("forbidden")[0], "refused")
        self.assertEqual(tuple(schema_compat.REQUIRED), schema_compat.CLASSES)

    def test_given_bump(self):
        gb = schema_compat.given_bump
        self.assertEqual([gb("1.0", "1.0"), gb("1.0", "1.3"), gb("1.4", "2.0"), gb("1.4", "2.9")],
                         ["none", "minor", "major", "major"])
        self.assertEqual([gb("1.4", "1.3"), gb("2.0", "1.9")], ["invalid", "invalid"])
        self.assertTrue(schema_compat.bump_satisfies("major", "minor"))
        self.assertFalse(schema_compat.bump_satisfies("minor", "major"))
        self.assertFalse(schema_compat.bump_satisfies("major", "refused"))
        self.assertFalse(schema_compat.bump_satisfies("invalid", "none"))

    def test_class_order_and_name_check(self):
        self.assertEqual(schema_compat.CLASSES, ("identical", "additive", "backward_only", "breaking", "forbidden"))
        row = self.rows[0]
        other = dict(row["new"], name="content.demo.other")
        with self.assertRaises(ValueError):
            schema_compat.classify(row["old"], other)

    def test_normalize_is_idempotent_and_accepts_both_forms(self):
        for row in self.rows:
            once = schemalib.normalize(row["new"])
            self.assertEqual(schemalib.normalize(once), once)

    def test_integer_widening_is_by_range(self):
        def t(width):
            return {"name": "a.b", "kind": "tagged", "version": "1.0", "domain": "a.b", "record_class": "none",
                    "state_class": "document", "element": width}
        self.assertEqual(schema_compat.classify(t("u8"), t("u64")), "backward_only")
        self.assertEqual(schema_compat.classify(t("i8"), t("i16")), "backward_only")
        self.assertEqual(schema_compat.classify(t("u16"), t("u8")), "breaking")
        self.assertEqual(schema_compat.classify(t("u8"), t("i8")), "breaking")
        self.assertEqual(schema_compat.classify(t("f32"), t("f64")), "breaking")


if __name__ == "__main__":
    unittest.main()
