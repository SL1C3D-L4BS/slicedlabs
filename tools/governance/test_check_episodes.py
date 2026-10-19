"""Unit tests for cmd_check_episodes.py (registry-check), cmd_stats.py and the real registry."""

import contextlib
import copy
import io
import os
import shutil
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_episodes as chk  # noqa: E402
import cmd_gen_episodes as gen  # noqa: E402
import cmd_stats  # noqa: E402
import slib  # noqa: E402
import test_gen_episodes as tg  # noqa: E402


def run(module, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = module.main(["x"] + argv)
    return status, out.getvalue(), err.getvalue()


class RealRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = chk.load_context(slib.ROOT)

    def test_real_registry_is_clean(self):
        self.assertEqual(chk.check(self.ctx), [])

    def test_golden_numbers(self):
        reg = self.ctx["reg"]
        self.assertEqual(len(reg["episodes"]), 840)
        self.assertEqual(len(reg["volumes"]), 42)
        self.assertEqual([(p["first"], p["last"]) for p in reg["parts"]],
                         [(1, 100), (101, 200), (201, 300), (301, 400), (401, 500), (501, 640), (641, 840)])
        self.assertEqual(sum(1 for e in reg["episodes"] if e["summary"] is None), 301)
        self.assertEqual(self.ctx["plan_actual"], self.ctx["plan_pinned"])

    def test_waiver_and_no_invented_dependencies(self):
        reg = self.ctx["reg"]
        self.assertEqual(reg["lifecycle"]["waivers"], [
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0003", "first": 41, "last": 48, "max_status": "Demonstrated"}])
        self.assertTrue(all(e["depends_on"] == [] for e in reg["episodes"]))
        self.assertEqual(sum(1 for v in reg["volumes"] if v["depends_on_volumes"]), 40)

    def test_mutation_selftest_passes(self):
        status, out, _ = run(chk, ["--mutation-selftest"])
        self.assertEqual(status, 0, out)
        self.assertIn("each named", out)

    def test_every_mutation_is_caught_individually(self):
        for label, tag, mutate in chk.MUTATIONS:
            reg = copy.deepcopy(self.ctx["reg"])
            mutate(reg)
            tags = {t for t, _, _ in chk.check(self.ctx, reg)}
            self.assertIn(tag, tags, label)

    def test_no_evidence_mutation_survives_an_advanced_registry(self):
        reg = copy.deepcopy(self.ctx["reg"])
        ep = reg["episodes"][70]  # SL-EP-071 already Demonstrated with real evidence
        ep["status"] = "Demonstrated"
        ep["history"] = chk._hist(
            ("Planned", "Specified"),
            ("Specified", "InProgress"),
            ("InProgress", "Verifying"),
            ("Verifying", "Demonstrated"),
        )
        ep["work_orders"] = ["WO-052"]
        ep["completion"] = {
            "benchmarks": [], "evidence": ["evidence/WO-052.json"], "exceptions": [],
            "open_defects": [], "revision": "0" * 40,
        }
        chk._m_no_evidence(reg)
        tags = {t for t, _, _ in chk.check(self.ctx, reg)}
        self.assertIn("R-NO-EVIDENCE", tags)

    def test_cli_prints_wall_time(self):
        status, out, _ = run(chk, [])
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"in \d+\.\d\ds wall")

    def test_usage_error_and_missing_registry(self):
        self.assertEqual(run(chk, ["--bogus"])[0], 2)
        self.assertEqual(run(chk, ["--registry", "/nonexistent/registry.json"])[0], 2)

    def test_stats_render_is_deterministic_and_totals_match(self):
        a, b = cmd_stats.render(self.ctx["reg"]), cmd_stats.render(copy.deepcopy(self.ctx["reg"]))
        self.assertEqual(a, b)
        self.assertIn("840 episodes, 42 volumes, 7 parts", a)
        by_state = next(line for line in a.splitlines() if line.startswith("by state: "))
        counts = [int(part.rsplit(" ", 1)[1]) for part in by_state[len("by state: "):].split(", ")]
        self.assertEqual(sum(counts), 840)
        rows = [line for line in a.splitlines() if line.startswith("SL-VOL-")]
        self.assertEqual(len(rows), 42)
        for row in rows:
            self.assertEqual(sum(int(x) for x in row.split()[2:2 + len(slib.STATES)]), 20, row)
        status, out, _ = run(cmd_stats, [])
        self.assertEqual((status, out), (0, a))
        self.assertEqual(run(cmd_stats, ["--registry", "/nonexistent.json"])[0], 2)


class MiniRegistryTests(unittest.TestCase):
    def setUp(self):
        self.root = tg.make_root()
        self.addCleanup(shutil.rmtree, self.root, True)
        status, out, err = run(gen, ["--root", self.root])
        self.assertEqual(status, 0, out + err)

    def test_golden_asserts_flag_a_small_registry(self):
        ctx = chk.load_context(self.root)
        tags = [t for t, _, _ in chk.check(ctx)]
        self.assertIn("GOLDEN", tags)
        self.assertNotIn("R-SCHEMA", tags)
        self.assertNotIn("LOCK", tags)

    def test_cli_exits_one_with_findings(self):
        status, out, _ = run(chk, ["--root", self.root])
        self.assertEqual(status, 1)
        self.assertIn("GOLDEN /episodes: expected 840 episodes, found 6", out)
        self.assertIn("registry-check: FAIL", out)

    def test_lock_drift_is_a_finding(self):
        ctx = chk.load_context(self.root)
        findings = chk.check(ctx, lock_text="0" * 64 + "\n")
        self.assertIn("LOCK", [t for t, _, _ in findings])

    def test_accepted_stub_must_be_an_object(self):
        ctx = chk.load_context(self.root)
        ctx["accepted"] = []
        self.assertIn("ACCEPTED", [t for t, _, _ in chk.check(ctx)])


if __name__ == "__main__":
    unittest.main()
