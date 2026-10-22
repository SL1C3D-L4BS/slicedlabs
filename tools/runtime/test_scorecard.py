"""Tests of tools/runtime/scorecard.py (WO-100; RT-INV-GATE-07, SL-ARCH-003 ARCH3-R171).

Each case writes clean gate evidence into a throwaway directory, plants one bad input and checks that the scorecard
fails naming that measure; coverage is injected so the cases do not depend on the repository's contracts state.
"""

import contextlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
_spec = importlib.util.spec_from_file_location("runtime_scorecard", os.path.join(HERE, "scorecard.py"))
scorecard = importlib.util.module_from_spec(_spec)
sys.modules["runtime_scorecard"] = scorecard
_spec.loader.exec_module(scorecard)

FULL = ({"bound": 150, "total": 150, "ratio": 1.0, "errors": 0, "status": "pass"}, None)


def clean_inputs(gate):
    docs = {
        "memreport.json": {"format": "sl-runtime-memreport/1", "leaks": []},
        "demo-report.json": {"format": "sl-runtime-gate/1",
                             "recovery": {"fault_tick": 450, "restored_tick": 300, "hash_equal": True}},
        "f5.json": {"format": "sl-runtime-f5/1", "mutations": 37, "bypassing": 0, "enabled": True},
        "stress-matrix.json": {"format": "sl-runtime-stress-matrix/1",
                               "cells": [{"cell": c, "pass": True} for c in scorecard.CELLS]},
        "hash-matrix.json": {"format": "sl-runtime-hash-matrix/1", "missing": [],
                             "rows": [{"workers": w, "equal": True} for w in (1, 2, 4, 8, 16)]},
    }
    for name, doc in docs.items():
        with open(os.path.join(gate, name), "w", encoding="utf-8") as fh:
            json.dump(doc, fh)


class ScorecardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="scorecard-")
        self.gate = os.path.join(self.tmp, "gate")
        self.san = os.path.join(self.tmp, "san")
        os.makedirs(self.gate)
        os.makedirs(self.san)
        clean_inputs(self.gate)
        self.log("asan-mem.log", "1: [doctest] Status: SUCCESS!\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def log(self, name, text):
        with open(os.path.join(self.san, name), "a", encoding="utf-8") as fh:
            fh.write(text)

    def edit(self, name, fn):
        path = os.path.join(self.gate, name)
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        fn(doc)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)

    def score(self, coverage=FULL):
        return scorecard.score(self.gate, self.san, self.tmp, coverage)

    def failed(self, coverage=FULL):
        card, failures = self.score(coverage)
        return [f.split(":", 1)[0] for f in failures], card

    def test_rt_inv_gate_07_clean_inputs_pass(self):
        card, failures = self.score()
        self.assertEqual(failures, [])
        self.assertEqual(card["format"], scorecard.OUTPUT_FORMAT)
        for name in scorecard.MEASURES:
            self.assertEqual(card[name]["status"], "pass", name)

    def test_rt_inv_gate_07_a_leak_is_named(self):
        self.edit("memreport.json", lambda d: d["leaks"].append({"tag": "events", "site": "a.cpp:1", "size": 8}))
        names, card = self.failed()
        self.assertEqual(names, ["leaks"])
        self.assertEqual(card["leaks"]["count"], 1)

    def test_rt_inv_gate_07_a_sanitizer_report_is_named(self):
        self.log("tsan-job.log", "WARNING: ThreadSanitizer: data race (pid=1)\n")
        names, card = self.failed()
        self.assertEqual(names, ["sanitizer_reports"])
        self.assertEqual(card["sanitizer_reports"]["by_sanitizer"], {"asan": 0, "tsan": 1})

    def test_rt_inv_gate_07_a_divergence_is_named(self):
        self.edit("hash-matrix.json", lambda d: d["rows"][2].update(equal=False))
        self.assertEqual(self.failed()[0], ["divergences"])
        clean_inputs(self.gate)
        self.edit("demo-report.json", lambda d: d["recovery"].update(hash_equal=False))
        self.assertEqual(self.failed()[0], ["divergences"])

    def test_rt_inv_gate_07_an_f5_bypass_is_named(self):
        self.edit("f5.json", lambda d: d.update(bypassing=1))
        self.assertEqual(self.failed()[0], ["f5_bypassing"])
        self.edit("f5.json", lambda d: d.update(bypassing=0, enabled=False))
        self.assertEqual(self.failed()[0], ["f5_bypassing"])

    def test_rt_inv_gate_07_a_stress_failure_is_named(self):
        self.edit("stress-matrix.json", lambda d: d["cells"][4].update({"pass": False}))
        names, card = self.failed()
        self.assertEqual(names, ["stress_failures"])
        self.assertEqual(card["stress_failures"]["cells"], ["S5"])
        self.edit("stress-matrix.json", lambda d: d["cells"].pop())
        self.assertEqual(self.failed()[1]["stress_failures"]["missing"], ["S10"])

    def test_rt_inv_gate_07_coverage_below_one_is_named(self):
        low = ({"bound": 148, "total": 150, "ratio": 0.986667, "errors": 4, "status": "fail"},
               "invariant_coverage: 148 of 150 invariants bound, 4 strict errors (threshold 1.0)")
        self.assertEqual(self.failed(low)[0], ["invariant_coverage"])

    def test_rt_inv_gate_07_optional_inputs_are_not_run_never_pass(self):
        os.remove(os.path.join(self.gate, "hash-matrix.json"))
        shutil.rmtree(self.san)
        card, failures = self.score()
        self.assertEqual(failures, [])
        self.assertEqual(card["sanitizer_reports"]["status"], "not_run")
        self.assertIsNone(card["divergences"]["cells"])

    def test_rt_inv_gate_07_a_missing_required_input_fails_as_not_run(self):
        os.remove(os.path.join(self.gate, "memreport.json"))
        card, failures = self.score()
        self.assertEqual(card["leaks"]["status"], "not_run")
        self.assertTrue(failures[0].startswith("leaks: not_run: memreport.json is missing"), failures)

    def test_rt_inv_gate_07_main_writes_the_scorecard_and_names_each_failure(self):
        self.edit("memreport.json", lambda d: d["leaks"].append({"tag": "events", "site": "a.cpp:1", "size": 8}))
        out = os.path.join(self.tmp, "out", "scorecard.json")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(scorecard.main(["--gate", self.gate, "--san", self.san, "--out", out]), 1)
        with open(out, encoding="utf-8") as fh:
            card = json.load(fh)
        self.assertTrue(card["failures"][0].startswith("leaks:"))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(scorecard.main(["extra"]), 2)


if __name__ == "__main__":
    unittest.main()
