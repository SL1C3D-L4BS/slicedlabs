"""Tests of the Volume 04 scorecard (WO-062): a clean conformance input passes and writes every measure; an F2 or F5
numerator above 0, a compiled-out ledger, coverage below 1.0, gate 1 above 0, a failed type or too many validator
invocations each fail naming the measure.

The input is built here in the format fabric_conformance_gate1_test writes, so the tests need no C++ build;
`just fabric-scorecard` runs the scorecard on the real input.
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scorecard  # noqa: E402


def clean_input():
    return {
        "format": scorecard.INPUT_FORMAT, "types": 47, "types_failed": 0,
        "f2": {"numerator": 0, "denominator": 10}, "f5": {"enabled": True, "mutations": 30, "bypassing": 0},
        "gate1": {"numerator": 0, "denominator": 10}, "commits": 30, "objects": 197, "validation_passes": 30,
        "validators": 8, "validator_invocations": 240, "changed_records": 39,
    }


class ScorecardTest(unittest.TestCase):
    def test_clean_input_passes_with_every_measure(self):
        card, failures = scorecard.score(clean_input(), 165, 165)
        self.assertEqual(failures, [])
        self.assertEqual(card["f2"], {"numerator": 0, "denominator": 10})
        self.assertEqual(card["f5"], {"numerator": 0, "denominator": 30, "enabled": True})
        self.assertEqual(card["invariant_coverage"]["ratio"], 1.0)
        self.assertAlmostEqual(card["objects_per_commit"], 6.567)
        self.assertEqual(card["validator_invocations"]["bound"], 312)

    def test_each_threshold_fails_naming_its_measure(self):
        cases = [
            (("f5", "bypassing"), 1, "F5:"),
            (("f2", "numerator"), 1, "F2:"),
            (("f5", "enabled"), False, "F5: the ledger is compiled out"),
            (("gate1", "numerator"), 2, "gate 1:"),
            (("types_failed",), 1, "types:"),
            (("validator_invocations",), 400, "validators:"),
        ]
        for keys, value, want in cases:
            with self.subTest(keys=keys):
                inp = clean_input()
                target = inp
                for k in keys[:-1]:
                    target = target[k]
                target[keys[-1]] = value
                _card, failures = scorecard.score(inp, 165, 165)
                self.assertEqual(len(failures), 1, failures)
                self.assertTrue(failures[0].startswith(want), failures)
        _card, failures = scorecard.score(clean_input(), 164, 165)
        self.assertEqual(failures, ["coverage: 164 of 165 invariants bound (threshold 1.0)"])

    def test_malformed_input_is_refused(self):
        for bad in ({"format": "x"}, dict(clean_input(), commits=-1), dict(clean_input(), f5={"enabled": 1})):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                scorecard.score(bad, 1, 1)

    def test_main_writes_the_scorecard_and_exits_1_on_f5(self):
        tmp = tempfile.mkdtemp()
        try:
            inp = clean_input()
            inp["f5"]["bypassing"] = 1
            path = os.path.join(tmp, "in.json")
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(inp, fh)
            out = os.path.join(tmp, "scorecard.json")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = scorecard.main([path, "--out", out])
            self.assertEqual(code, 1)
            self.assertIn("FAIL F5:", buf.getvalue())
            with open(out, encoding="utf-8") as fh:
                card = json.load(fh)
            self.assertEqual(card["f5"]["numerator"], 1)
            self.assertEqual(card["format"], scorecard.OUTPUT_FORMAT)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(scorecard.main([]), 2)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
