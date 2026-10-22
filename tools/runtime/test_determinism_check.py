"""Tests for runtime-determinism's checker (WO-099, RT-INV-GATE-05, SL-ARCH-003 ARCH3-R170): an all-equal hash
matrix passes and writes sl-runtime-hash-matrix/1, a single divergent cell is rejected naming its workers, mode,
variant and tick, a missing cell is rejected, and a differing second compiler is recorded without changing the exit
status. No subprocess: main() runs in-process against temp run directories.
"""

import hashlib
import importlib.util
import io
import json
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("runtime_determinism_check", os.path.join(_here, "determinism_check.py"))
dc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dc)

TICKS = 12
RESUME = 6


def _hash(text):
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


def _timeline(salt=""):
    return [_hash("tick-%d%s" % (t, salt)) for t in range(1, TICKS + 1)]


class DeterminismCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="det-check-")
        self.runs = os.path.join(self.tmp, "runs")
        self.out = os.path.join(self.tmp, "gate", "hash-matrix.json")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write_run(self, build, workers, mode, compiler="clang", fresh=None, resume=None, replay=None):
        line = _timeline()
        doc = {
            "format": "sl-runtime-hashes/1", "compiler": compiler, "seed": 1, "ticks": TICKS, "entities": 100,
            "workers": workers, "mode": mode, "resume_tick": RESUME,
            "fresh": fresh if fresh is not None else line,
            "resume": resume if resume is not None else line[RESUME:],
            "replay": replay if replay is not None else line,
        }
        d = os.path.join(self.runs, "%s-w%d-%s" % (build, workers, mode))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hashes.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f)

    def write_matrix(self, build="debug", workers=(1, 2, 4, 8, 16)):
        for w in workers:
            for m in ("parallel", "serial"):
                self.write_run(build, w, m)

    def run_check(self, *extra):
        out = io.StringIO()
        code = dc.main(["--runs", self.runs, "--primary", "debug", "--out", self.out, *extra], out=out)
        return code, out.getvalue()

    def matrix(self):
        with open(self.out, encoding="utf-8") as f:
            return json.load(f)

    def test_rt_inv_gate_05_accepts_all_equal_matrix(self):
        self.write_matrix()
        code, text = self.run_check()
        self.assertEqual(code, 0, text)
        self.assertIn("determinism-check: ok (30 rows", text)
        doc = self.matrix()
        self.assertEqual(doc["format"], "sl-runtime-hash-matrix/1")
        self.assertTrue(doc["all_equal"])
        self.assertIsNone(doc["first_divergence"])
        self.assertEqual(len(doc["rows"]), 5 * 2 * 3)
        cells = {(r["workers"], r["mode"], r["variant"]) for r in doc["rows"]}
        self.assertEqual(len(cells), 30)
        resume = next(r for r in doc["rows"] if r["variant"] == "resume")
        self.assertEqual(resume["first_tick"], RESUME + 1)
        self.assertEqual(resume["ticks"], TICKS - RESUME)
        self.assertEqual(doc["reference"]["workers"], 1)
        self.assertEqual(doc["reference"]["mode"], "parallel")

    def test_rt_inv_gate_05_rejects_single_divergent_cell(self):
        self.write_matrix()
        bad = _timeline()
        bad[8] = _hash("other")  # tick 9
        self.write_run("debug", 8, "serial", replay=bad)
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("divergence at workers=8 mode=serial variant=replay tick=9", text)
        doc = self.matrix()
        self.assertFalse(doc["all_equal"])
        self.assertEqual(doc["first_divergence"], {"workers": 8, "mode": "serial", "variant": "replay", "tick": 9})
        self.assertEqual(sum(1 for r in doc["rows"] if not r["equal"]), 1)

    def test_rt_inv_gate_05_rejects_divergent_resume_at_its_tick(self):
        self.write_matrix()
        bad = _timeline()[RESUME:]
        bad[-1] = _hash("late")
        self.write_run("debug", 2, "parallel", resume=bad)
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("workers=2 mode=parallel variant=resume tick=%d" % TICKS, text)

    def test_rt_inv_gate_05_names_first_divergence_in_matrix_order(self):
        self.write_matrix()
        self.write_run("debug", 16, "parallel", fresh=_timeline("x"))
        self.write_run("debug", 4, "serial", fresh=_timeline("y"))
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("divergence at workers=4 mode=serial variant=fresh tick=1", text)

    def test_rt_inv_gate_05_rejects_missing_cell(self):
        self.write_matrix(workers=(1, 2, 4, 8))
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("missing run workers=16 mode=parallel", text)
        self.assertIn("missing run workers=16 mode=serial", text)

    def test_rt_inv_gate_05_subset_of_workers(self):
        self.write_run("debug", 1, "parallel")
        self.write_run("debug", 8, "parallel", fresh=_timeline("neg"))
        code, text = self.run_check("--workers", "1,8", "--modes", "parallel")
        self.assertEqual(code, 1, text)
        self.assertIn("divergence at workers=8 mode=parallel variant=fresh tick=1", text)

    def test_rt_inv_gate_05_cross_compiler_recorded_only(self):
        self.write_matrix()
        self.write_run("gcc-release", 1, "parallel", compiler="gcc", fresh=_timeline("gcc"))
        code, text = self.run_check()
        self.assertEqual(code, 0, text)
        self.assertIn("cross_compiler (recorded only): differs", text)
        cross = self.matrix()["cross_compiler"]
        self.assertTrue(cross["recorded_only"])
        self.assertFalse(cross["all_equal"])
        self.assertEqual(cross["runs"][0]["compiler"], "gcc")
        self.assertEqual(cross["runs"][0]["divergent_tick"], 1)

    def test_rt_inv_gate_05_inconsistent_scene_is_input_error(self):
        self.write_matrix(workers=(1,))
        self.write_run("debug", 2, "parallel", fresh=_timeline()[:-1])
        code, text = self.run_check("--workers", "1")
        self.assertEqual(code, 2, text)
        self.assertIn("fresh holds 11 hashes for 12 ticks", text)

    def test_rt_inv_gate_05_usage(self):
        out = io.StringIO()
        self.assertEqual(dc.main(["--runs", self.runs], out=out), 2)
        self.assertEqual(dc.main(["--runs", self.runs, "--primary", "debug", "--workers", "0"], out=out), 2)
        code, text = self.run_check()
        self.assertEqual(code, 2, text)
        self.assertIn("not a directory", text)


if __name__ == "__main__":
    unittest.main()
