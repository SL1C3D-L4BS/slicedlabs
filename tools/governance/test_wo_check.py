"""Unit tests for wo-check (cmd_wo_check.py), with fixtures in tests/governance/fixtures/wo."""

import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_wo_check  # noqa: E402
import slgov  # noqa: E402

FIXTURES = os.path.join(slgov.ROOT, "tests", "governance", "fixtures", "wo")


def read(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as fh:
        return fh.read()


def recipes():
    return slgov.parse_justfile(read("justfile"))


def check(text, name="WO-901.md", ids=("SL-EP-001", "SL-EP-002", "SL-EP-003")):
    return cmd_wo_check.check_work_order(text, name, recipes(), set(ids) if ids is not None else None)


def rules(errors):
    return {r for r, _ in errors}


class ParserTests(unittest.TestCase):
    def test_good_fixture_parses_to_template_shapes(self):
        meta, body = cmd_wo_check.parse_work_order(read("good.md"))
        self.assertEqual(meta["risk"], ["governed"])
        self.assertEqual(meta["acceptance"][1], {"cmd": "just test unit", "expect_exit": 0})
        self.assertEqual(meta["budget"], {"tokens": 1000000, "usd": 8, "rounds": 2})
        self.assertIs(meta["touches_gates"], False)
        self.assertTrue(body.startswith("Traces: SL-EP-002 SL-EP-003"))

    def test_parse_errors(self):
        for bad in ("no front matter\n", "---\nid: WO-1\n", "---\nid: a\nid: b\n---\n", "---\n\tid: a\n---\n"):
            with self.subTest(bad):
                with self.assertRaises((cmd_wo_check.WoParseError, slgov.FrontMatterError)):
                    cmd_wo_check.parse_work_order(bad)

    def test_unparseable_is_front_error(self):
        self.assertEqual(rules(check("---\nid: a\nid: b\n---\n")), {"R-WO-FRONT"})


class CheckTests(unittest.TestCase):
    def test_good_fixture_passes(self):
        self.assertEqual(check(read("good.md")), [])

    def test_each_bad_fixture_fails_with_its_rule(self):
        names = sorted(n for n in os.listdir(FIXTURES) if n.startswith("bad-"))
        self.assertGreaterEqual(len(names), 6)
        for name in names:
            expected = name[len("bad-") : -len(".md")].upper()
            with self.subTest(name):
                self.assertIn(expected, rules(check(read(name))))

    def test_registry_check_skipped_when_ids_none(self):
        self.assertEqual(check(read("bad-r-wo-registry.md"), ids=None), [])

    def test_id_must_match_file_name(self):
        self.assertIn("R-WO-FRONT", rules(check(read("good.md"), name="WO-902.md")))

    def test_class_caps(self):
        cases = (("S", 300000, 3, True), ("S", 300001, 3, False), ("S", 300000, 4, False), ("M", 1000000, 8, True),
                 ("M", 1000000, 9, False), ("L", 1500000, 20, True), ("L", 1500001, 20, False))
        for cls, tokens, usd, ok in cases:
            text = read("good.md").replace("class: M", "class: " + cls).replace("tokens: 1000000", "tokens: %d" % tokens)
            text = text.replace("usd: 8", "usd: %d" % usd)
            with self.subTest((cls, tokens, usd)):
                self.assertEqual("R-WO-BUDGET" not in rules(check(text)), ok)

    def test_rounds_at_most_two(self):
        self.assertIn("R-WO-BUDGET", rules(check(read("good.md").replace("rounds: 2", "rounds: 3"))))
        self.assertNotIn("R-WO-BUDGET", rules(check(read("good.md").replace("rounds: 2", "rounds: 0"))))

    def test_acceptance_command_forms(self):
        good = read("good.md")
        for cmd in ("just doc-check; rm -rf x", "make test", "just", "just doc-check && just test", "just $(x)", "just test 'a b'"):
            text = good.replace('cmd: "just doc-check"', 'cmd: "%s"' % cmd)
            with self.subTest(cmd):
                self.assertIn("R-WO-ACCEPT", rules(check(text)))

    def test_acceptance_needs_expect_exit(self):
        text = read("good.md").replace("    expect_exit: 0\n", "", 1)
        self.assertIn("R-WO-ACCEPT", rules(check(text)))

    def test_traces_line_variants(self):
        good = read("good.md")
        for body in ("", "Traces:\n", "Traces: SL-EP-002 SL-EP-002\n", "Traces: EP-002\n", "Traces: SL-EP-002\nTraces: SL-EP-003\n"):
            text = good.split("---\n")[0] + "---\n" + good.split("---\n")[1] + "---\n" + body
            with self.subTest(body):
                self.assertIn("R-WO-TRACES", rules(check(text)))

    def test_auto_approver_refused_for_risk_flags(self):
        text = read("good.md").replace("approver: founder", "approver: auto")
        self.assertIn("R-WO-FRONT", rules(check(text)))
        text = text.replace("risk: [governed]", "risk: [none]")
        self.assertEqual(check(text), [])


class RunTests(unittest.TestCase):
    def make_root(self, with_registry):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root)
        os.makedirs(os.path.join(root, "work-orders"))
        shutil.copy(os.path.join(FIXTURES, "justfile"), os.path.join(root, "justfile"))
        shutil.copy(os.path.join(FIXTURES, "good.md"), os.path.join(root, "work-orders", "WO-901.md"))
        if with_registry:
            os.makedirs(os.path.join(root, "registry"))
            shutil.copy(os.path.join(FIXTURES, "registry.json"), os.path.join(root, "registry", "SL-REG-002-episodes.json"))
        return root

    def run_check(self, root):
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = cmd_wo_check.run(root)
        return code, buf.getvalue()

    def test_absent_registry_is_a_note_not_a_failure(self):
        code, out = self.run_check(self.make_root(False))
        self.assertEqual(code, 0)
        self.assertIn("cross-check skipped", out)

    def test_present_registry_is_enforced(self):
        root = self.make_root(True)
        self.assertEqual(self.run_check(root)[0], 0)
        shutil.copy(os.path.join(FIXTURES, "bad-r-wo-registry.md"), os.path.join(root, "work-orders", "WO-901.md"))
        code, out = self.run_check(root)
        self.assertEqual(code, 1)
        self.assertIn("work-orders/WO-901.md:1: R-WO-REGISTRY", out)


if __name__ == "__main__":
    unittest.main()
