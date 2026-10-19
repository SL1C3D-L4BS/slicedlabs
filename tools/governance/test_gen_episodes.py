"""Unit tests for cmd_gen_episodes.py, on a small synthetic plan excerpt (not the real master plan)."""

import contextlib
import copy
import hashlib
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_gen_episodes as gen  # noqa: E402
import slib  # noqa: E402

MINI_PLAN = (
    "# **Mini plan**\n"
    "\n"
    "### **1.4 Release scope and delivery order**\n"
    "\n"
    "| Milestone | Episodes | What ships |\n"
    "| :---- | :---- | :---- |\n"
    "| Foundation | 001\u2013004 | Base |\n"
    "| Sliced Labs 1.0 | 005\u2013006 | Everything |\n"
    "\n"
    "### **4.1 Volumes**\n"
    "\n"
    "| Part | Volume | Episodes | Title |\n"
    "| :---- | :---- | :---- | :---- |\n"
    "| I Foundations | 01 | 001\u2013002 | First |\n"
    "| I | 02 | 003\u2013004 | Second |\n"
    "| II Later | 03 | 005\u2013006 | Third |\n"
    "\n"
    "### **4.2 Volume dependency graph**\n"
    "\n"
    "flowchart LR  \n"
    "  V01\\[01 One\\] \\-\\-\\> V02\\[02 Two\\]  \n"
    "  V01 \\-\\-\\> V03\\[03 Three \\+ more\\] & V02  \n"
    "  V02 & V03 \\-\\-\\> V04  \n"
    "The graph is the minimum hard order.\n"
    "\n"
    "### **5.1 Arc-to-volume mapping**\n"
    "\n"
    "| Kohi arc | Approx. Kohi episodes | Sliced Labs episodes | Differs |\n"
    "| :---- | :---- | :---- | :---- |\n"
    "| Setup | \\#000\u2013\\#002 | 001\u2013002 | Own build |\n"
    "| Logging | \\#003 | 002, 005 | Typed fields |\n"
    "\n"
    "### **6.2 Episode lifecycle**\n"
    "\n"
    "stateDiagram-v2  \n"
    "  \\[\\*\\] \\-\\-\\> Planned  \n"
    "  Planned \\-\\-\\> Specified: spec  \n"
    "  Specified \\-\\-\\> InProgress: deps  \n"
    "  InProgress \\-\\-\\> Verifying: done  \n"
    "  Verifying \\-\\-\\> InProgress: tests fail  \n"
    "  Verifying \\-\\-\\> Demonstrated: tests pass  \n"
    "  Demonstrated \\-\\-\\> Accepted: evidence  \n"
    "  Accepted \\-\\-\\> Reopened: regression  \n"
    "  Reopened \\-\\-\\> InProgress  \n"
    "  Verifying \\-\\-\\> Blocked: dependency regressed  \n"
    "  Blocked \\-\\-\\> InProgress  \n"
    "\n"
    "## **Part I \u2014 Foundations (episodes 001\u2013004)**\n"
    "\n"
    "### **Volume 01 \u2014 First volume**\n"
    "\n"
    "Goal: define the thing. Kohi \\#000.\n"
    "\n"
    "* **001** Define a \\#thing \u2014 with \\+ and snake\\_case \u2014 and a second dash.  \n"
    "* **002** Title only.  \n"
    "\n"
    "Deliverable: nothing.\n"
    "\n"
    "### **Volume 02 \u2014 Second volume**\n"
    "\n"
    "* **003** Plain title \u2014 plain summary.  \n"
    "* **004** Another title-only bullet.\n"
    "\n"
    "## **Part II \u2014 Later (episodes 005\u2013006)**\n"
    "\n"
    "### **Volume 03 \u2014 Third volume**\n"
    "\n"
    "* **005** Fifth \u2014 fifth summary.  \n"
    "* **006** Sixth \u2014 sixth summary.  \n"
    "\n"
    "## **Elsewhere**\n"
    "\n"
    "* **007** A stray list item after the volumes should not be an episode.\n"
)
# the mini 4.2 names V04, which has no volume heading: registry tests use a variant without that edge
MINI_PLAN_OK = MINI_PLAN.replace("  V02 & V03 \\-\\-\\> V04  \n", "")


def make_root(plan_text=MINI_PLAN_OK, pin=None):
    root = tempfile.mkdtemp(prefix="gen-episodes-")
    for rel, content in (
        (gen.PLAN_REL, plan_text),
        (gen.PIN_REL, (pin or hashlib.sha256(plan_text.encode("utf-8")).hexdigest()) + "  x\n"),
        (gen.SEED_RELS[0], '{"episodes": {}, "schema": "%s"}\n' % gen.SEED_SCHEMA),
        (gen.SEED_RELS[1], '{"episodes": {}, "schema": "%s"}\n' % gen.SEED_SCHEMA),
    ):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)
    return root


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = gen.main(["x"] + argv)
    return status, out.getvalue(), err.getvalue()


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.plan = gen.parse_plan(MINI_PLAN_OK)

    def test_counts_and_stray_bullet_ignored(self):
        self.assertEqual([e["number"] for e in self.plan["episodes"]], [1, 2, 3, 4, 5, 6])
        self.assertEqual(len(self.plan["volumes"]), 3)
        self.assertEqual([(p["number"], p["first"], p["last"]) for p in self.plan["parts"]], [(1, 1, 4), (2, 5, 6)])

    def test_unescape_and_first_dash_split(self):
        e = self.plan["episodes"][0]
        self.assertEqual(e["title"], "Define a #thing")
        self.assertEqual(e["summary"], "with + and snake_case \u2014 and a second dash")

    def test_title_only_has_null_summary_and_no_trailing_period(self):
        e = self.plan["episodes"][1]
        self.assertEqual((e["title"], e["summary"]), ("Title only", None))

    def test_volume_goal_and_part(self):
        v = self.plan["volumes"]
        self.assertEqual(v[0]["goal"], "define the thing. Kohi #000.")
        self.assertIsNone(v[1]["goal"])
        self.assertEqual([x["part"] for x in v], [1, 1, 2])

    def test_edges_chains_and_groups(self):
        self.assertEqual(gen.parse_volume_edges(MINI_PLAN.split("\n")), [(1, 2), (1, 3), (2, 4), (3, 4)])

    def test_chain_edges(self):
        lines = ["### **4.2 x**", "  V09 \\-\\-\\> V27\\[27 G\\] \\-\\-\\> V28\\[28 F\\]  ", "### **4.3 y**"]
        self.assertEqual(gen.parse_volume_edges(lines), [(9, 27), (27, 28)])

    def test_kohi_and_milestones_and_transitions(self):
        self.assertEqual(self.plan["kohi"][2], ["Setup (#000\u2013#002)", "Logging (#003)"])
        self.assertEqual(self.plan["kohi"][5], ["Logging (#003)"])
        self.assertEqual(self.plan["milestones"], [("Foundation", 1, 4), ("1.0", 5, 6)])
        self.assertEqual(self.plan["transitions"][:2], [("Planned", "Specified"), ("Specified", "InProgress")])
        self.assertEqual(sorted(self.plan["transitions"]), sorted(slib.fsm_edges()))

    def test_range_mismatch_with_table_is_refused(self):
        broken = MINI_PLAN_OK.replace("| I | 02 | 003\u2013004 |", "| I | 02 | 003\u2013005 |")
        with self.assertRaises(gen.ParseError):
            gen.parse_plan(broken)

    def test_missing_section_is_refused(self):
        with self.assertRaises(gen.ParseError):
            gen.parse_plan(MINI_PLAN_OK.replace("### **5.1 Arc", "### **5.9 Arc"))


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.plan = gen.parse_plan(MINI_PLAN_OK)
        self.reg = gen.build_registry(self.plan, "a" * 64, {})

    def test_registry_passes_the_rule_engine(self):
        self.assertEqual(slib.check_registry(self.reg, plan_sha256="a" * 64), [])

    def test_volume_dependencies_from_the_graph_only(self):
        deps = {v["id"]: v["depends_on_volumes"] for v in self.reg["volumes"]}
        self.assertEqual(deps, {"SL-VOL-01": [], "SL-VOL-02": ["SL-VOL-01"], "SL-VOL-03": ["SL-VOL-01"]})
        self.assertTrue(all(e["depends_on"] == [] for e in self.reg["episodes"]))

    def test_episode_record_defaults(self):
        e = self.reg["episodes"][0]
        self.assertEqual(e["status"], "Planned")
        self.assertEqual(e["history"], [])
        self.assertIsNone(e["completion"])
        self.assertEqual(e["meaning_sha256"], slib.meaning_sha256(e["title"], e["summary"]))
        self.assertEqual(e["release"], {"milestone": "Foundation", "platforms": []})
        self.assertEqual(self.reg["episodes"][4]["release"]["milestone"], "1.0")
        self.assertEqual(self.reg["episodes"][1]["kohi_ref"], "Setup (#000\u2013#002); Logging (#003)")

    def test_lifecycle_and_waiver(self):
        self.assertEqual(self.reg["lifecycle"]["states"], list(slib.STATES))
        self.assertEqual(self.reg["lifecycle"]["waivers"], [
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0003", "first": 41, "last": 48, "max_status": "Demonstrated"},
            {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0021", "first": 81, "last": 100, "max_status": "Demonstrated"}])

    def test_volume_gates(self):
        def gate(adr, first, last, state, rfirst, rlast):
            return {"adr": adr, "first": first, "last": last, "state": state, "requires_first": rfirst,
                    "requires_last": rlast, "requires_status": "Accepted"}

        self.assertEqual(self.reg["lifecycle"]["gates"], [
            gate("ADR-0020", 61, 80, "Accepted", 41, 60),
            gate("ADR-0021", 81, 100, "Accepted", 21, 40),
            gate("ADR-0021", 121, 140, "InProgress", 21, 40)])
        self.assertEqual(self.reg["lifecycle"]["gates"], [dict(g) for g in gen.GATES])
        self.assertEqual(slib.schema_findings(self.reg, "sl-reg-002"), [])
        bad = copy.deepcopy(self.reg)
        bad["lifecycle"]["gates"][0]["extra"] = 1
        self.assertTrue(slib.schema_findings(bad, "sl-reg-002"))

    def test_gates_do_not_change_the_lock(self):
        r = copy.deepcopy(self.reg)
        r["lifecycle"]["gates"] = []
        self.assertEqual(gen.lock_digest(r), gen.lock_digest(self.reg))

    def test_gate_helpers(self):
        self.assertEqual([gen.gate_rank(s) for s in ("Planned", "Blocked", "Reopened", "Accepted")], [0, 3, 5, 5])
        gate = gen.GATES[0]
        status = {n: "Accepted" for n in range(41, 61)}
        self.assertIsNone(gen.gate_unmet(gate, status))
        status[44] = "Demonstrated"
        self.assertEqual(gen.gate_unmet(gate, status), 44)
        del status[44]
        self.assertEqual(gen.gate_unmet(gate, status), 44)

    def test_dumps_is_deterministic_with_trailing_newline(self):
        a, b = gen.dumps(self.reg), gen.dumps(copy.deepcopy(self.reg))
        self.assertEqual(a, b)
        self.assertTrue(a.endswith("}\n") and not a.endswith("\n\n"))
        self.assertTrue(a.startswith('{\n "amendments"'))

    def test_seed_must_agree_with_the_plan(self):
        seed = {"SL-EP-001": {"decision_status": "Locked", "kohi_ref": "Wrong", "release": {"milestone": "Foundation"}}}
        with self.assertRaises(gen.ParseError):
            gen.build_registry(self.plan, "a" * 64, seed)
        seed["SL-EP-001"]["kohi_ref"] = "Setup (#000\u2013#002)"
        reg = gen.build_registry(self.plan, "a" * 64, seed)
        self.assertEqual(reg["episodes"][0]["decision_status"], "Locked")
        with self.assertRaises(gen.ParseError):
            gen.build_registry(self.plan, "a" * 64, {"SL-EP-099": seed["SL-EP-001"]})

    def test_lock_covers_titles_and_ranges_only(self):
        base = gen.lock_digest(self.reg)
        r = copy.deepcopy(self.reg)
        r["episodes"][2]["summary"] = "changed"
        self.assertEqual(gen.lock_digest(r), base)
        r["episodes"][2]["title"] = "renamed"
        self.assertNotEqual(gen.lock_digest(r), base)
        r = copy.deepcopy(self.reg)
        r["volumes"][0]["last"] = 3
        self.assertNotEqual(gen.lock_digest(r), base)


class GenerateAndCheckTests(unittest.TestCase):
    def setUp(self):
        self.root = make_root()
        self.addCleanup(shutil.rmtree, self.root, True)

    def reg_path(self):
        return os.path.join(self.root, gen.REG_REL)

    def read_reg(self):
        with open(self.reg_path(), encoding="utf-8") as fh:
            return fh.read()

    def test_generate_then_refuse_overwrite_then_force(self):
        status, out, _ = run(["--root", self.root])
        self.assertEqual(status, 0, out)
        for rel in (gen.REG_REL, gen.LOCK_REL, gen.ACCEPTED_REL):
            self.assertTrue(os.path.exists(os.path.join(self.root, rel)), rel)
        with open(os.path.join(self.root, gen.ACCEPTED_REL), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "{}\n")
        before = self.read_reg()
        status, _, err = run(["--root", self.root])
        self.assertEqual(status, 2)
        self.assertIn("refusing to overwrite", err)
        self.assertEqual(self.read_reg(), before)
        self.assertEqual(run(["--root", self.root, "--force"])[0], 0)
        self.assertEqual(self.read_reg(), before)

    def test_check_passes_on_fresh_output(self):
        run(["--root", self.root])
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 0, out)

    def test_check_fails_on_a_silent_title_change(self):
        run(["--root", self.root])
        reg = slib.load_json(self.reg_path())
        reg["episodes"][2]["title"] = "Quietly renamed"
        reg["episodes"][2]["meaning_sha256"] = slib.meaning_sha256("Quietly renamed", reg["episodes"][2]["summary"])
        with open(self.reg_path(), "w", encoding="utf-8") as fh:
            fh.write(gen.dumps(reg))
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 1)
        self.assertIn("SL-EP-003 title", out)

    def test_check_accepts_a_change_covered_by_an_amendment(self):
        run(["--root", self.root])
        reg = slib.load_json(self.reg_path())
        ep = reg["episodes"][3]
        old = ep["meaning_sha256"]
        ep["summary"] = "Now with a summary"
        ep["meaning_sha256"] = slib.meaning_sha256(ep["title"], ep["summary"])
        ep["amendments"] = [{"date": "2026-10-18", "note": "n", "impact": "i", "adr": None, "from_sha256": old,
                             "to_sha256": ep["meaning_sha256"], "traceability_updated": True}]
        with open(self.reg_path(), "w", encoding="utf-8") as fh:
            fh.write(gen.dumps(reg))
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 0, out)

    def test_check_fails_on_a_missing_or_extra_episode_and_wrong_volume(self):
        run(["--root", self.root])
        reg = slib.load_json(self.reg_path())
        del reg["episodes"][5]
        reg["episodes"][0]["volume"] = "SL-VOL-02"
        with open(self.reg_path(), "w", encoding="utf-8") as fh:
            fh.write(gen.dumps(reg))
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 1)
        self.assertIn("SL-EP-006 is in the plan but not in the registry", out)
        self.assertIn("SL-EP-001 volume", out)

    def test_check_fails_when_the_plan_no_longer_matches_its_pin(self):
        run(["--root", self.root])
        with open(os.path.join(self.root, gen.PLAN_REL), "a", encoding="utf-8") as fh:
            fh.write("\nedited\n")
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 1)
        self.assertIn("pins", out)

    def test_check_fails_when_the_lock_drifts(self):
        run(["--root", self.root])
        with open(os.path.join(self.root, gen.LOCK_REL), "w", encoding="utf-8") as fh:
            fh.write("0" * 64 + "\n")
        status, out, _ = run(["--check", "--root", self.root])
        self.assertEqual(status, 1)
        self.assertIn("immutable projection", out)

    def test_usage_errors(self):
        self.assertEqual(run(["--bogus"])[0], 2)
        self.assertEqual(run(["--check", "--force"])[0], 2)


if __name__ == "__main__":
    unittest.main()
