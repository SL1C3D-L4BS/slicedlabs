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
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0003", "first": 41, "last": 48, "max_status": "Demonstrated"},
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0021", "first": 81, "last": 100, "max_status": "Demonstrated"}])
        self.assertEqual(reg["lifecycle"]["gates"], [dict(g) for g in gen.GATES])
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

    def gate_tags(self, statuses):
        """R-VOL-GATE findings after setting {episode number: status} on a copy of the real registry."""
        reg = copy.deepcopy(self.ctx["reg"])
        for n in range(21, 141):
            reg["episodes"][n - 1]["status"] = "Planned"
        for n, status in statuses.items():
            reg["episodes"][n - 1]["status"] = status
        return chk.vol_gate_findings(reg)

    def test_vol_gate_names_the_gate_and_the_unmet_episode(self):
        found = self.gate_tags({n: "Accepted" for n in range(42, 61)} | {41: "Demonstrated", 61: "Accepted"})
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][0], "R-VOL-GATE")
        for text in ("SL-EP-061", "ADR-0020", "SL-EP-041", "Demonstrated"):
            self.assertIn(text, found[0][2])

    def test_vol_gate_allows_a_met_gate_and_lower_states(self):
        accepted = {n: "Accepted" for n in range(41, 61)}
        self.assertEqual(self.gate_tags(accepted | {61: "Accepted", 62: "Demonstrated"}), [])
        # Volume 05 may be Demonstrated with Volume 02 untouched, but not Accepted (ADR-0021)
        self.assertEqual(self.gate_tags({81: "Demonstrated", 82: "InProgress", 83: "Verifying"}), [])
        self.assertEqual(len(self.gate_tags({81: "Accepted"})), 1)
        volume_02 = {n: "Accepted" for n in range(21, 41)}
        self.assertEqual(self.gate_tags(volume_02 | {81: "Accepted", 121: "InProgress"}), [])

    def test_vol_gate_volume_07_starts_only_after_volume_02(self):
        self.assertEqual(self.gate_tags({121: "Specified"}), [])
        for status in ("InProgress", "Verifying", "Demonstrated", "Accepted", "Blocked", "Reopened"):
            found = self.gate_tags({121: status})
            self.assertEqual(len(found), 1, status)
            self.assertIn("ADR-0021", found[0][2])
            self.assertIn("SL-EP-021", found[0][2])
        accepted = {n: "Accepted" for n in range(21, 41)}
        self.assertEqual(self.gate_tags(accepted | {121: "InProgress"}), [])
        accepted[33] = "Demonstrated"
        self.assertIn("SL-EP-033", self.gate_tags(accepted | {121: "InProgress"})[0][2])

    def test_gates_must_equal_the_generators(self):
        for mutate in (lambda g: g.pop(), lambda g: g.reverse(), lambda g: g[0].update(first=62)):
            reg = copy.deepcopy(self.ctx["reg"])
            mutate(reg["lifecycle"]["gates"])
            findings = [f for f in chk.check(self.ctx, reg) if f[1] == "/lifecycle/gates"]
            self.assertEqual([f[0] for f in findings], ["GOLDEN"])
        reg = copy.deepcopy(self.ctx["reg"])
        del reg["lifecycle"]["gates"]
        self.assertIn(("GOLDEN", "/lifecycle/gates"), [(t, p) for t, p, _ in chk.check(self.ctx, reg)])

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
