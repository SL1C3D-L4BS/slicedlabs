"""Unit tests for ep-advance (cmd_ep_advance.py), with fixtures in tests/governance/fixtures/wo."""

import contextlib
import copy
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_ep_advance as adv  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

FIXTURES = os.path.join(slgov.ROOT, "tests", "governance", "fixtures", "wo")
GOOD = open(os.path.join(FIXTURES, "good.md"), encoding="utf-8").read()
REG_PATH = "registry/SL-REG-002-episodes.json"


def wo_text(traces):
    return GOOD.replace("Traces: SL-EP-002 SL-EP-003", "Traces: " + traces)


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        for d in ("work-orders", "evidence", "registry"):
            os.makedirs(os.path.join(self.root, d))
        with open(os.path.join(FIXTURES, "registry.json"), encoding="utf-8") as fh:
            self.reg = json.load(fh)
        self.write_registry(self.reg)
        self.write_wo("WO-901", "SL-EP-002 SL-EP-003")
        self.write_wo("WO-902", "SL-EP-004 SL-EP-005")

    def write_registry(self, reg):
        with open(os.path.join(self.root, REG_PATH), "w", encoding="utf-8") as fh:
            fh.write(adv.dump_registry(reg))

    def write_wo(self, wo, traces):
        with open(os.path.join(self.root, "work-orders", wo + ".md"), "w", encoding="utf-8") as fh:
            fh.write(wo_text(traces).replace("id: WO-901", "id: " + wo))

    def write_evidence(self, name, **overrides):
        wo = name
        with open(os.path.join(self.root, "work-orders", wo + ".md"), "rb") as fh:
            sha = hashlib.sha256(fh.read()).hexdigest()
        rec = {"wo": wo, "wo_sha256": sha, "merge_commit": "a" * 40, "branch_tip_tree": "b" * 40,
               "gate_log_sha256": None, "pass": True, "date": "2026-10-17"}
        rec.update(overrides)
        with open(os.path.join(self.root, "evidence", wo + ".json"), "w", encoding="utf-8") as fh:
            json.dump(rec, fh)

    def registry_text(self):
        with open(os.path.join(self.root, REG_PATH), encoding="utf-8") as fh:
            return fh.read()

    def go(self, ep, state, wo, adr=None, date="2026-10-18"):
        """Run the command; return (exit code, stderr)."""
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            code = adv.run(self.root, ep, state, wo, date, adr)
        return code, err.getvalue()

    def episode(self, ep_id):
        return next(e for e in json.loads(self.registry_text())["episodes"] if e["id"] == ep_id)


class TransitionTests(Base):
    def test_legal_transition_appends_history(self):
        code, _ = self.go("SL-EP-003", "Verifying", "WO-901")
        self.assertEqual(code, 0)
        ep = self.episode("SL-EP-003")
        self.assertEqual(ep["status"], "Verifying")
        self.assertEqual(ep["history"][-1], {"from": "InProgress", "to": "Verifying", "date": "2026-10-18", "note": "WO-901"})
        self.assertEqual(slib.check_registry(json.loads(self.registry_text())), [])

    def test_illegal_transition_is_refused_and_registry_untouched(self):
        before = self.registry_text()
        code, err = self.go("SL-EP-004", "Accepted", "WO-902")
        self.assertEqual(code, 1)
        self.assertIn("R-LIFECYCLE", err)
        self.assertEqual(self.registry_text(), before)

    def test_every_6_2_edge_is_accepted_by_the_validator_and_nothing_else(self):
        for old in slib.STATES:
            for new in slib.STATES:
                reg = copy.deepcopy(self.reg)
                ep = reg["episodes"][3]
                ep["status"] = old
                legal = slib.can_transition(old, new)
                if not legal:
                    with self.assertRaises(adv.Refused):
                        adv.advance(reg, self.root, ep["id"], new, "WO-902", "2026-10-18")
                    continue
                if new in slib.EVIDENCE_STATES:
                    self.write_evidence("WO-902")
                adv.advance(reg, self.root, ep["id"], new, "WO-902", "2026-10-18")
                self.assertEqual(ep["status"], new)

    def test_unknown_episode_state_and_wo(self):
        self.assertEqual(self.go("SL-EP-099", "Specified", "WO-902")[0], 1)
        self.assertIn("unknown state", self.go("SL-EP-004", "Done", "WO-902")[1])
        self.assertEqual(self.go("SL-EP-004", "Specified", "WO-777")[0], 1)

    def test_wo_must_trace_the_episode(self):
        code, err = self.go("SL-EP-004", "Specified", "WO-901")
        self.assertEqual(code, 1)
        self.assertIn("R-WO-TRACE", err)

    def test_wo_is_added_to_work_orders(self):
        self.assertEqual(self.go("SL-EP-004", "Specified", "WO-902")[0], 0)
        self.assertEqual(self.episode("SL-EP-004")["work_orders"], ["WO-902"])

    def test_missing_registry_is_a_clear_error(self):
        os.remove(os.path.join(self.root, REG_PATH))
        code, err = self.go("SL-EP-004", "Specified", "WO-902")
        self.assertEqual(code, 1)
        self.assertIn("does not exist yet", err)


class EvidenceTests(Base):
    def test_demonstrated_without_evidence_is_refused(self):
        self.go("SL-EP-003", "Verifying", "WO-901")
        before = self.registry_text()
        code, err = self.go("SL-EP-003", "Demonstrated", "WO-901")
        self.assertEqual(code, 1)
        self.assertIn("R-NO-EVIDENCE", err)
        self.assertEqual(self.registry_text(), before)

    def test_evidence_is_checked(self):
        self.go("SL-EP-003", "Verifying", "WO-901")
        for label, overrides in (("pass false", {"pass": False}), ("pass string", {"pass": "true"}),
                                 ("sha mismatch", {"wo_sha256": "0" * 64}), ("bad sha", {"wo_sha256": "xyz"}),
                                 ("other wo", {"wo": "WO-902"})):
            self.write_evidence("WO-901", **overrides)
            with self.subTest(label):
                code, err = self.go("SL-EP-003", "Demonstrated", "WO-901")
                self.assertEqual(code, 1)
                self.assertIn("R-NO-EVIDENCE", err)

    def test_edited_work_order_invalidates_evidence(self):
        self.go("SL-EP-003", "Verifying", "WO-901")
        self.write_evidence("WO-901")
        with open(os.path.join(self.root, "work-orders", "WO-901.md"), "a", encoding="utf-8") as fh:
            fh.write("\nedited after the evidence was recorded\n")
        self.assertIn("changed after", self.go("SL-EP-003", "Demonstrated", "WO-901")[1])

    def test_good_evidence_demonstrates_and_records_completion(self):
        self.go("SL-EP-003", "Verifying", "WO-901")
        self.write_evidence("WO-901")
        self.assertEqual(self.go("SL-EP-003", "Demonstrated", "WO-901")[0], 0)
        ep = self.episode("SL-EP-003")
        self.assertEqual(ep["status"], "Demonstrated")
        self.assertEqual(ep["completion"]["evidence"], ["evidence/WO-901.json"])
        self.assertEqual(ep["completion"]["revision"], "a" * 40)

    def test_accepted_needs_evidence_and_keeps_existing_completion(self):
        self.assertIn("R-NO-EVIDENCE", self.go("SL-EP-002", "Accepted", "WO-901")[1])
        self.write_evidence("WO-901")
        self.assertEqual(self.go("SL-EP-002", "Accepted", "WO-901")[0], 0)
        ep = self.episode("SL-EP-002")
        self.assertEqual(ep["status"], "Accepted")
        self.assertEqual(ep["completion"]["evidence"], ["evidence/WO-002.json", "evidence/WO-901.json"])


class DependencyTests(Base):
    def test_started_state_needs_accepted_dependencies(self):
        code, err = self.go("SL-EP-005", "InProgress", "WO-902")
        self.assertEqual(code, 1)
        self.assertIn("R-DEP-ACCEPTED", err)
        self.assertIn("SL-EP-003 (InProgress)", err)

    def test_waiver_needs_matching_adr(self):
        for adr in ("ADR-0009", None):
            code, err = self.go("SL-EP-005", "InProgress", "WO-902", adr)
            self.assertEqual(code, 1)
        self.assertIn("no waiver ADR-0009", self.go("SL-EP-005", "InProgress", "WO-902", "ADR-0009")[1])
        code, _ = self.go("SL-EP-005", "InProgress", "WO-902", "ADR-0003")
        self.assertEqual(code, 0)
        self.assertEqual(self.episode("SL-EP-005")["history"][-1]["note"], "WO-902 (waiver ADR-0003)")

    def test_waiver_does_not_reach_accepted(self):
        for state in ("InProgress", "Verifying", "Demonstrated"):
            if state == "Demonstrated":
                self.write_evidence("WO-902")
            self.assertEqual(self.go("SL-EP-005", state, "WO-902", "ADR-0003")[0], 0, state)
        code, err = self.go("SL-EP-005", "Accepted", "WO-902", "ADR-0003")
        self.assertEqual(code, 1)
        self.assertIn("waives only up to Demonstrated", err)

    def test_waiver_does_not_cover_other_episodes(self):
        reg = copy.deepcopy(self.reg)
        reg["lifecycle"]["waivers"][0]["first"] = 4
        reg["lifecycle"]["waivers"][0]["last"] = 4
        self.write_registry(reg)
        self.assertIn("no waiver", self.go("SL-EP-005", "InProgress", "WO-902", "ADR-0003")[1])


class ArgumentTests(unittest.TestCase):
    def test_parse_args(self):
        ok = adv.parse_args(["x", "SL-EP-012", "Accepted", "WO-012", "--adr", "ADR-0003", "--date", "2026-10-17"])
        self.assertEqual(ok[:5], ("SL-EP-012", "Accepted", "WO-012", "2026-10-17", "ADR-0003"))
        for bad in (["SL-EP-12", "Accepted", "WO-012"], ["SL-EP-012", "Accepted", "WO-12"], ["SL-EP-012", "Accepted"],
                    ["SL-EP-012", "Accepted", "WO-012", "--adr", "ADR-3"], ["SL-EP-012", "Accepted", "WO-012", "--date", "2026-13-40"],
                    ["SL-EP-012", "Accepted", "WO-012", "--force"], ["SL-EP-012", "Accepted", "WO-012", "--adr"]):
            with self.subTest(bad):
                self.assertIsInstance(adv.parse_args(["x"] + bad), str)

    def test_default_date_is_today(self):
        parsed = adv.parse_args(["x", "SL-EP-012", "Accepted", "WO-012"])
        self.assertRegex(parsed[3], r"^\d{4}-\d{2}-\d{2}$")


if __name__ == "__main__":
    unittest.main()
