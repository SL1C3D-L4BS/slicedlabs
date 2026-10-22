"""Tests of tools/runtime/stress_report.py (WO-098; no invariant row, run by `just tools-test`)."""

import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stress_report as sr  # noqa: E402

STEMS = sorted({stem for stem, _ in sr.CELLS.values()} | {"stress_failpoints"})


def testcase(stem, time="1.5", status="run", out="", failure=None):
    body = "<system-out>%s</system-out>" % out
    if failure is not None:
        body = '<failure message="%s"/>' % failure + body
    return '<testcase name="runtime_%s_test" classname="runtime_%s_test" time="%s" status="%s">%s</testcase>' % (
        stem,
        stem,
        time,
        status,
        body,
    )


def junit(cases=None, skip=()):
    cases = cases or {}
    parts = [cases.get(stem, testcase(stem)) for stem in STEMS if stem not in skip]
    return '<?xml version="1.0" encoding="UTF-8"?><testsuite name="x" tests="%d">%s</testsuite>' % (
        len(parts),
        "".join(parts),
    )


class StressReportTest(unittest.TestCase):
    def run_main(self, text, extra=(), environ=None, previous=None):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "junit.xml")
            dst = os.path.join(tmp, "gate", "stress-matrix.json")
            with open(src, "w", encoding="utf-8") as f:
                f.write(text)
            if previous is not None:
                os.makedirs(os.path.dirname(dst))
                with open(dst, "w", encoding="utf-8") as f:
                    json.dump(previous, f)
            out, err = io.StringIO(), io.StringIO()
            code = sr.main(["--junit", src, "--out", dst, *extra], environ=environ or {}, out=out, err=err)
            doc = None
            if os.path.exists(dst):
                with open(dst, encoding="utf-8") as f:
                    doc = json.load(f)
            return code, doc, out.getvalue(), err.getvalue()

    def test_all_pass_writes_ten_schema_valid_cells(self):
        code, doc, out, _ = self.run_main(junit())
        self.assertEqual(code, 0)
        self.assertEqual(doc["format"], "sl-runtime-stress-matrix/1")
        self.assertEqual(doc["scale"], 1.0)
        self.assertEqual([c["cell"] for c in doc["cells"]], ["S%d" % i for i in range(1, 11)])
        self.assertTrue(all(c["pass"] and c["preset"] == "debug" for c in doc["cells"]))
        self.assertEqual(doc["cells"][6]["test"], "tests/integration/runtime/stress_det_matrix_test.cpp")
        self.assertEqual(doc["cells"][0]["seed"], sr.CELLS["S1"][1])
        self.assertEqual(sr.slib.validate(doc, self.schema()), [])
        self.assertIn("wrote", out)

    def schema(self):
        return sr.slib.load_json(os.path.join(sr.ROOT, sr.SCHEMA_PATH))

    def test_failed_cell_takes_its_rerun_seed(self):
        output = (
            "TEST CASE:  RT-INV-STRESS-03: S4 allocator churn\n"
            "  logged: seed 11; rerun: SL_RUNTIME_SOAK_SEED=11 ctest\n"
            "TEST CASE:  RT-INV-STRESS-03: S5 RFP0 at every allocation index\n"
            "  logged: seed 4242; rerun: SL_RUNTIME_SOAK_SEED=4242 ctest\n"
        )
        cases = {"stress_alloc": testcase("stress_alloc", time="7.25", status="fail", out=output, failure="x")}
        code, doc, out, _ = self.run_main(junit(cases))
        self.assertEqual(code, 0)
        cells = {c["cell"]: c for c in doc["cells"]}
        self.assertFalse(cells["S5"]["pass"])
        self.assertFalse(cells["S4"]["pass"])
        self.assertEqual(cells["S5"]["seed"], 4242)
        self.assertEqual(cells["S4"]["seed"], 11)
        self.assertEqual(cells["S5"]["wall_s"], 7.25)
        self.assertTrue(cells["S6"]["pass"])
        self.assertIn("S5 debug FAIL", out)

    def test_soak_seed_replaces_the_fixed_seed(self):
        code, doc, _, _ = self.run_main(junit(), environ={"SL_RUNTIME_SOAK_SEED": "987654321"})
        self.assertEqual(code, 0)
        self.assertTrue(all(c["seed"] == 987654321 for c in doc["cells"]))

    def test_missing_stress_file_is_exit_1(self):
        code, doc, _, err = self.run_main(junit(skip=("stress_events",)))
        self.assertEqual(code, 1)
        self.assertIsNone(doc)
        self.assertIn("runtime_stress_events_test", err)

    def test_failpoints_file_is_not_required(self):
        code, doc, _, _ = self.run_main(junit(skip=("stress_failpoints",)))
        self.assertEqual(code, 0)
        self.assertEqual(len(doc["cells"]), 10)

    def test_malformed_junit_is_exit_1(self):
        code, doc, _, err = self.run_main("<testsuite><testcase")
        self.assertEqual(code, 1)
        self.assertIsNone(doc)
        self.assertIn("malformed JUnit", err)

    def test_bad_time_is_exit_1(self):
        code, _, _, err = self.run_main(junit({"stress_jobs": testcase("stress_jobs", time="soon")}))
        self.assertEqual(code, 1)
        self.assertIn("not a number", err)

    def test_merge_keeps_other_presets_and_replaces_its_own(self):
        _, first, _, _ = self.run_main(junit(), extra=("--preset", "ci-tsan"))
        self.assertEqual(first["scale"], 0.1)
        old_debug = dict(first["cells"][0], preset="debug", wall_s=99.0)
        previous = {"format": sr.FORMAT, "scale": 0.1, "cells": first["cells"] + [old_debug]}
        code, doc, _, _ = self.run_main(junit(), extra=("--merge",), previous=previous)
        self.assertEqual(code, 0)
        presets = [c["preset"] for c in doc["cells"]]
        self.assertEqual(presets.count("ci-tsan"), 10)
        self.assertEqual(presets.count("debug"), 10)
        self.assertNotIn(99.0, [c["wall_s"] for c in doc["cells"]])
        self.assertEqual(sr.slib.validate(doc, self.schema()), [])

    def test_without_merge_the_file_is_replaced(self):
        previous = {"format": sr.FORMAT, "scale": 0.1, "cells": [{"cell": "S1", "preset": "ci-asan"}]}
        code, doc, _, _ = self.run_main(junit(), previous=previous)
        self.assertEqual(code, 0)
        self.assertEqual({c["preset"] for c in doc["cells"]}, {"debug"})

    def test_scale_from_environment_and_argument(self):
        _, doc, _, _ = self.run_main(junit(), environ={"SL_RUNTIME_STRESS_SCALE": "0.5"})
        self.assertEqual(doc["scale"], 0.5)
        _, doc, _, _ = self.run_main(junit(), extra=("--scale", "2"))
        self.assertEqual(doc["scale"], 2.0)

    def test_usage_is_exit_2(self):
        for argv in (["--preset", "release"], ["--scale", "-1"], ["--scale", "x"], ["--bogus"], ["--junit"]):
            err = io.StringIO()
            self.assertEqual(sr.main(argv, environ={}, out=io.StringIO(), err=err), 2, argv)
            self.assertIn("usage", err.getvalue())

    def test_missing_junit_is_exit_1(self):
        err = io.StringIO()
        code = sr.main(["--junit", "/nonexistent/junit.xml", "--out", "/nonexistent/out.json"], environ={},
                       out=io.StringIO(), err=err)
        self.assertEqual(code, 1)
        self.assertIn("junit.xml", err.getvalue())


if __name__ == "__main__":
    unittest.main()
