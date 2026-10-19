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
import cmd_gen_episodes as gen  # noqa: E402
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
        self.assertEqual(
            ep["history"][-1], {"from": "InProgress", "to": "Verifying", "date": "2026-10-18", "note": "WO-901"}
        )
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


class VolumeGateTests(Base):
    """R-VOL-GATE (lifecycle.gates): ADR-0020 cap on 061-080, ADR-0021 on 081-100 and 121-140."""

    def setUp(self):
        super().setUp()
        self.write_wo("WO-903", "SL-EP-061 SL-EP-081 SL-EP-121")

    def gated(self, statuses):
        """The fixture registry with episodes {number: status}, the real waivers and the real gates."""
        template = self.reg["episodes"][3]  # SL-EP-004, Planned, no dependencies
        episodes = []
        for number, status in sorted(statuses.items()):
            ep = copy.deepcopy(template)
            ep["id"], ep["number"], ep["status"] = "SL-EP-%03d" % number, number, status
            episodes.append(ep)
        reg = copy.deepcopy(self.reg)
        reg["episodes"] = episodes
        reg["lifecycle"]["waivers"] = [dict(w) for w in gen.WAIVERS]
        reg["lifecycle"]["gates"] = [dict(g) for g in gen.GATES]
        return reg

    def accepted(self, first, last, **others):
        statuses = {n: "Accepted" for n in range(first, last + 1)}
        statuses.update({int(k[1:]): v for k, v in others.items()})
        return statuses

    def test_adr_0020_cap_refuses_accepted_while_041_is_demonstrated(self):
        reg = self.gated(self.accepted(42, 60, n41="Demonstrated", n61="Demonstrated"))
        self.write_evidence("WO-903")
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(reg, self.root, "SL-EP-061", "Accepted", "WO-903", "2026-10-19")
        message = str(ctx.exception)
        for text in ("R-VOL-GATE", "SL-EP-061", "ADR-0020", "SL-EP-041", "Demonstrated", "no --adr bypass"):
            self.assertIn(text, message)
        self.assertEqual(next(e for e in reg["episodes"] if e["number"] == 61)["status"], "Demonstrated")
        next(e for e in reg["episodes"] if e["number"] == 41)["status"] = "Accepted"
        adv.advance(reg, self.root, "SL-EP-061", "Accepted", "WO-903", "2026-10-19")
        self.assertEqual(next(e for e in reg["episodes"] if e["number"] == 61)["status"], "Accepted")

    def test_adr_0020_cap_names_the_first_unmet_episode(self):
        statuses = self.accepted(41, 60, n47="Verifying", n55="Planned", n61="Demonstrated")
        self.write_evidence("WO-903")
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(self.gated(statuses), self.root, "SL-EP-061", "Accepted", "WO-903", "2026-10-19")
        self.assertIn("SL-EP-047 is Verifying", str(ctx.exception))
        del statuses[47]
        statuses[55] = "Accepted"
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(self.gated(statuses), self.root, "SL-EP-061", "Accepted", "WO-903", "2026-10-19")
        self.assertIn("SL-EP-047 is missing", str(ctx.exception))

    def test_volume_04_below_the_cap_is_not_gated(self):
        reg = self.gated({61: "Verifying", 41: "Planned"})
        self.write_evidence("WO-903")
        adv.advance(reg, self.root, "SL-EP-061", "Demonstrated", "WO-903", "2026-10-19")
        reg = self.gated({61: "Verifying", 41: "Planned"})
        adv.advance(reg, self.root, "SL-EP-061", "Blocked", "WO-903", "2026-10-19")  # never a gated target

    def test_volume_05_may_reach_demonstrated_but_not_accepted_before_volume_02(self):
        reg = self.gated({81: "Specified", 21: "Planned"})
        self.write_evidence("WO-903")
        for state in ("InProgress", "Verifying", "Demonstrated"):
            adv.advance(reg, self.root, "SL-EP-081", state, "WO-903", "2026-10-19")
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(reg, self.root, "SL-EP-081", "Accepted", "WO-903", "2026-10-19")
        for text in ("R-VOL-GATE", "ADR-0021", "SL-EP-021..SL-EP-040", "SL-EP-021 is Planned"):
            self.assertIn(text, str(ctx.exception))
        self.assertEqual(reg["episodes"][-1]["status"], "Demonstrated")
        reg = self.gated(self.accepted(21, 40, n81="Demonstrated"))
        adv.advance(reg, self.root, "SL-EP-081", "Accepted", "WO-903", "2026-10-19")
        self.assertEqual(reg["episodes"][-1]["status"], "Accepted")

    def test_volume_07_cannot_start_before_volume_02_is_accepted(self):
        reg = self.gated({121: "Planned"})
        adv.advance(reg, self.root, "SL-EP-121", "Specified", "WO-903", "2026-10-19")  # Specified is not gated
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(reg, self.root, "SL-EP-121", "InProgress", "WO-903", "2026-10-19")
        for text in ("R-VOL-GATE", "ADR-0021", "SL-EP-121", "InProgress", "SL-EP-021 is missing"):
            self.assertIn(text, str(ctx.exception))
        reg = self.gated(self.accepted(21, 40, n121="Specified", n33="Demonstrated"))
        self.assertIn("SL-EP-033 is Demonstrated", self.refusal(reg, "SL-EP-121", "InProgress"))
        reg = self.gated(self.accepted(21, 40, n121="Specified"))
        adv.advance(reg, self.root, "SL-EP-121", "InProgress", "WO-903", "2026-10-19")

    def refusal(self, reg, ep, state):
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(reg, self.root, ep, state, "WO-903", "2026-10-19")
        return str(ctx.exception)

    def test_adr_flag_does_not_bypass_a_gate(self):
        reg = self.gated({121: "Specified"})
        reg["lifecycle"]["waivers"].append(
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0021", "first": 121, "last": 140, "max_status": "Demonstrated"}
        )
        self.assertIn("R-VOL-GATE", self.refusal_adr(reg, "SL-EP-121", "InProgress", "ADR-0021"))

    def refusal_adr(self, reg, ep, state, adr):
        with self.assertRaises(adv.Refused) as ctx:
            adv.advance(reg, self.root, ep, state, "WO-903", "2026-10-19", adr)
        return str(ctx.exception)

    def test_cli_refusal_leaves_the_registry_untouched(self):
        self.write_registry(self.gated({121: "Specified", 21: "Demonstrated"}))
        before = self.registry_text()
        code, err = self.go("SL-EP-121", "InProgress", "WO-903")
        self.assertEqual(code, 1)
        self.assertIn("R-VOL-GATE", err)
        self.assertIn("SL-EP-021 is Demonstrated", err)
        self.assertEqual(self.registry_text(), before)

    def test_registry_without_gates_is_not_gated(self):
        reg = self.gated({121: "Specified"})
        del reg["lifecycle"]["gates"]
        adv.advance(reg, self.root, "SL-EP-121", "InProgress", "WO-903", "2026-10-19")

    def test_waiver_for_adr_0021(self):
        reg = self.gated({81: "Specified", 100: "Specified", 101: "Specified"})
        ep81, ep101 = reg["episodes"][0], reg["episodes"][2]
        self.assertEqual(adv.waiver_for(reg, ep81, "ADR-0021", "Demonstrated")["last"], 100)
        ep100 = reg["episodes"][1]
        self.assertEqual(adv.waiver_for(reg, ep100, "ADR-0021", "InProgress")["max_status"], "Demonstrated")
        with self.assertRaises(adv.Refused) as ctx:
            adv.waiver_for(reg, ep81, "ADR-0021", "Accepted")
        self.assertIn("ADR-0021 waives only up to Demonstrated", str(ctx.exception))
        for episode, adr in ((ep101, "ADR-0021"), (ep81, "ADR-0003")):
            with self.assertRaises(adv.Refused) as ctx:
                adv.waiver_for(reg, episode, adr, "InProgress")
            self.assertIn("no waiver %s" % adr, str(ctx.exception))


class ArgumentTests(unittest.TestCase):
    def test_parse_args(self):
        ok = adv.parse_args(["x", "SL-EP-012", "Accepted", "WO-012", "--adr", "ADR-0003", "--date", "2026-10-17"])
        self.assertEqual(ok[:5], ("SL-EP-012", "Accepted", "WO-012", "2026-10-17", "ADR-0003"))
        for bad in (
            ["SL-EP-12", "Accepted", "WO-012"],
            ["SL-EP-012", "Accepted", "WO-12"],
            ["SL-EP-012", "Accepted"],
            ["SL-EP-012", "Accepted", "WO-012", "--adr", "ADR-3"],
            ["SL-EP-012", "Accepted", "WO-012", "--date", "2026-13-40"],
            ["SL-EP-012", "Accepted", "WO-012", "--force"],
            ["SL-EP-012", "Accepted", "WO-012", "--adr"],
        ):
            with self.subTest(bad):
                self.assertIsInstance(adv.parse_args(["x"] + bad), str)

    def test_default_date_is_today(self):
        parsed = adv.parse_args(["x", "SL-EP-012", "Accepted", "WO-012"])
        self.assertRegex(parsed[3], r"^\d{4}-\d{2}-\d{2}$")


if __name__ == "__main__":
    unittest.main()
