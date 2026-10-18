"""Unit tests for cmd_views.py (registry-views) and cmd_plan_check.py (plan-check) (WO-011)."""

import contextlib
import csv
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_plan_check  # noqa: E402
import cmd_views  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

REAL = slgov.ROOT


class Tree(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        shutil.copy(os.path.join(REAL, cmd_views.REGISTRY_REL), self.mkdir("registry") + "/SL-REG-002-episodes.json")
        for rel in cmd_plan_check.DOCS.values():
            self.copy(rel)
        os.makedirs(os.path.join(self.root, "work-orders"))
        for name in os.listdir(os.path.join(REAL, "work-orders")):
            if name.endswith(".md"):
                self.copy("work-orders/" + name)
        cmd_views.write_views(self.root)
        self.reg = cmd_views.load_registry(self.root)

    def tearDown(self):
        self._tmp.cleanup()

    def mkdir(self, rel):
        path = os.path.join(self.root, *rel.split("/"))
        os.makedirs(path, exist_ok=True)
        return path

    def copy(self, rel):
        dst = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(REAL, *rel.split("/")), dst)

    def path(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def edit(self, rel, old, new):
        with open(self.path(rel), encoding="utf-8", newline="") as fh:
            text = fh.read()
        self.assertIn(old, text)
        with open(self.path(rel), "w", encoding="utf-8", newline="") as fh:
            fh.write(text.replace(old, new, 1))


class ViewsTest(Tree):
    def test_generation_is_deterministic(self):
        self.assertEqual(cmd_views.generate(self.reg), cmd_views.generate(self.reg))

    def test_fresh_views_pass(self):
        self.assertEqual(cmd_views.drift_errors(self.root), [])

    def test_csv_has_every_episode(self):
        rows = list(csv.reader(io.StringIO(cmd_views.episodes_csv(self.reg))))
        self.assertEqual(rows[0], ["id", "volume", "title", "status", "kohi_ref"])
        self.assertEqual(len(rows) - 1, 840)
        self.assertEqual(rows[41][:2], ["SL-EP-041", "SL-VOL-03"])

    def test_graph_has_one_edge_per_dependency(self):
        text = cmd_views.volume_graph_mmd(self.reg)
        edges = sum(len(v["depends_on_volumes"]) for v in self.reg["volumes"])
        self.assertEqual(sum(1 for line in text.splitlines() if " --> " in line), edges)
        self.assertIn("V01 --> V03", text)
        self.assertIn("V22 --> V25", text)

    def test_kohi_map_lists_every_kohi_episode_as_clean_room(self):
        text = cmd_views.kohi_map_md(self.reg)
        kohi = [e for e in self.reg["episodes"] if e["kohi_ref"]]
        self.assertEqual(text.count("| true |"), len(kohi))
        self.assertIn("| Architecture overview and setup (#000–#002) | 041-048 | 8 |", text)

    def test_ranges(self):
        self.assertEqual(cmd_views.ranges([130, 131, 132, 133, 147, 151]), "130-133, 147, 151")
        self.assertEqual(cmd_views.ranges([97, 108]), "097, 108")
        self.assertEqual(cmd_views.ranges([126, 127, 134]), "126, 127, 134")

    def test_neg_edited_view_is_drift_and_named(self):
        self.edit("registry/views/volumes.md", "| 03 | 1 |", "| 03 | 2 |")
        errors = cmd_views.drift_errors(self.root)
        self.assertEqual(len(errors), 1)
        self.assertIn("registry/views/volumes.md", errors[0])
        self.assertIn("R-VIEW-DRIFT", errors[0])

    def test_neg_missing_view_is_drift(self):
        os.remove(self.path("registry/views/episodes.csv"))
        errors = cmd_views.drift_errors(self.root)
        self.assertEqual(len(errors), 1)
        self.assertIn("episodes.csv", errors[0])
        self.assertIn("missing", errors[0])

    def test_neg_registry_change_makes_views_stale(self):
        self.edit("registry/SL-REG-002-episodes.json", '"status": "Planned"', '"status": "Specified"')
        names = [e.split(":")[0] for e in cmd_views.drift_errors(self.root)]
        self.assertEqual(names, ["registry/views/episodes.csv"])

    def test_view_hashes_feed_the_slib_rule(self):
        views = cmd_views.view_hashes(self.root)
        self.assertEqual(sorted(views), sorted(cmd_views.VIEW_NAMES))
        self.assertEqual(slib.rule_view_drift(self.reg, {"views": views}), [])
        self.edit("registry/views/kohi-map.md", "# Kohi", "# kohi")
        findings = slib.rule_view_drift(self.reg, {"views": cmd_views.view_hashes(self.root)})
        self.assertEqual([f["rule"] for f in findings], ["R-VIEW-DRIFT"])
        self.assertEqual(findings[0]["path"], "/views/kohi-map.md")

    def test_main_check_exit_codes(self):
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            self.assertEqual(cmd_views.main(["x", "--check", "--root", self.root]), 0)
            self.edit("registry/views/volume-graph.mmd", "flowchart LR", "flowchart TD")
            self.assertEqual(cmd_views.main(["x", "--check", "--root", self.root]), 1)
            self.assertEqual(cmd_views.main(["x", "--bogus"]), 2)
        self.assertIn("R-VIEW-DRIFT", sink.getvalue())

    def test_real_views_are_current(self):
        self.assertEqual(cmd_views.drift_errors(REAL), [])


class PlanCheckTest(Tree):
    def rules(self):
        return sorted({e.split(": ")[1] for e in cmd_plan_check.plan_errors(self.root)})

    def test_clean_tree_passes(self):
        self.assertEqual(cmd_plan_check.plan_errors(self.root), [])

    def test_real_tree_passes(self):
        self.assertEqual(cmd_plan_check.plan_errors(REAL), [])

    def test_neg_unknown_episode(self):
        self.edit("docs/plan/SL-PLAN-004-vertical-slices.md", "SL-EP-064, SL-EP-080", "SL-EP-064, SL-EP-9999")
        errors = cmd_plan_check.plan_errors(self.root)
        self.assertEqual(self.rules(), ["R-PLAN-EP"])
        self.assertIn("SL-EP-9999", errors[0])

    def test_neg_gate_episode_missing(self):
        path = "docs/plan/SL-PLAN-004-vertical-slices.md"
        with open(self.path(path), encoding="utf-8") as fh:
            text = fh.read()
        with open(self.path(path), "w", encoding="utf-8") as fh:
            fh.write(text.replace("SL-EP-640", "SL-EP-641"))
        errors = cmd_plan_check.plan_errors(self.root)
        self.assertEqual(self.rules(), ["R-PLAN-GATE"])
        self.assertIn("SL-EP-640", errors[0])

    def test_neg_graph_drift_and_reference(self):
        self.edit("registry/views/volume-graph.mmd", "V01 --> V03", "V02 --> V03")
        self.assertEqual(self.rules(), ["R-PLAN-GRAPH"])
        os.remove(self.path("registry/views/volume-graph.mmd"))
        self.assertEqual(self.rules(), ["R-PLAN-GRAPH"])

    def test_neg_vol03_missing_record(self):
        path = "docs/vol/SL-VOL-03-SPEC.md"
        with open(self.path(path), encoding="utf-8") as fh:
            lines = [ln for ln in fh.read().split("\n") if '"SL-EP-059"' not in ln]
        with open(self.path(path), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        errors = cmd_plan_check.plan_errors(self.root)
        self.assertIn("R-PLAN-VOL03", self.rules())
        self.assertTrue(any("SL-EP-059" in e for e in errors))

    def test_neg_vol03_wrong_work_order(self):
        self.edit("docs/vol/SL-VOL-03-SPEC.md", '"work_orders": ["WO-015", "WO-021"]', '"work_orders": ["WO-016", "WO-021"]')
        errors = cmd_plan_check.plan_errors(self.root)
        self.assertEqual(self.rules(), ["R-PLAN-VOL03"])
        self.assertTrue(any("WO-016" in e and "SL-EP-043" in e for e in errors))

    def test_neg_vol03_title_drift(self):
        self.edit("docs/vol/SL-VOL-03-SPEC.md", "Automated formatting", "Auto formatting")
        self.assertEqual(self.rules(), ["R-PLAN-VOL03"])

    def test_neg_kohi_table_drift(self):
        self.edit("docs/plan/SL-PLAN-005-kohi-alignment-map.md", "| 041-048 | 8 |", "| 041-047 | 7 |")
        self.assertEqual(self.rules(), ["R-PLAN-KOHI"])

    def test_neg_code_fence_in_clean_room_doc(self):
        self.edit("docs/plan/SL-PLAN-005-kohi-alignment-map.md", "## Open issues", "```\nvoid f();\n```\n\n## Open issues")
        self.assertEqual(self.rules(), ["R-PLAN-CLEAN-ROOM"])

    def test_neg_missing_document(self):
        os.remove(self.path("docs/plan/SL-PLAN-002-episode-dependency-graph.md"))
        self.assertEqual(self.rules(), ["R-PLAN-DOC"])


if __name__ == "__main__":
    unittest.main()
