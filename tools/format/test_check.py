"""Unit tests of tools/format/check.py (stdlib unittest, run by `just tools-test`)."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402


def kinds(findings):
    return sorted({f[0] for f in findings})


class TextRules(unittest.TestCase):
    def test_cmake_clean(self):
        self.assertEqual(check.check_cmake("a.cmake", "if(X)\n  message(\"y\")\nendif()\n"), [])

    def test_cmake_tab_trailing_and_final_newline(self):
        got = check.check_cmake("a.cmake", "if(X)\n\tmessage(1)  \nendif()")
        messages = " | ".join(f[3] for f in got)
        self.assertIn("tab used for indentation", messages)
        self.assertIn("trailing whitespace", messages)
        self.assertIn("no newline at end of file", messages)

    def test_cmake_odd_indent_outside_parentheses_only(self):
        self.assertTrue(check.check_cmake("a.cmake", "if(X)\n message(1)\nendif()\n"))
        self.assertEqual(check.check_cmake("a.cmake", "set(A\n    b\n   c)\n"), [])

    def test_python_long_line_and_syntax(self):
        self.assertTrue(check.check_python("a.py", "x = '%s'\n" % ("a" * 130)))
        got = check.check_python("a.py", "def f(:\n")
        self.assertIn("syntax error", got[0][3])

    def test_shell_tab_indent(self):
        self.assertTrue(check.check_shell("a.sh", "if true; then\n\techo\nfi\n"))
        self.assertEqual(check.check_shell("a.sh", "echo ok\n"), [])

    def test_fix_text(self):
        self.assertEqual(check.fix_text("a  \n\tb", True), "a\n  b\n")


class JsonRules(unittest.TestCase):
    def test_indent2_canonical(self):
        text = json.dumps({"a": [1, 2]}, indent=2) + "\n"
        self.assertEqual(check.check_json("deps/x.json", text), [])
        self.assertTrue(check.check_json("deps/x.json", '{"a":[1,2]}\n'))

    def test_episodes_use_indent1_sort_keys(self):
        rel = "registry/SL-REG-002-episodes.json"
        text = json.dumps({"b": 1, "a": 2}, indent=1, sort_keys=True) + "\n"
        self.assertEqual(check.check_json(rel, text), [])
        self.assertTrue(check.check_json("registry/other.json", text))

    def test_invalid_json_is_named(self):
        self.assertIn("invalid JSON", check.check_json("deps/x.json", "{")[0][3])


class FrontMatter(unittest.TestCase):
    KEYS = "id kind title status version owner episodes depends_on supersedes decisions".split()

    def doc(self, keys):
        return "---\n" + "".join("%s: x\n" % k for k in keys) + "---\n# T\n"

    def test_in_order(self):
        self.assertEqual(check.check_frontmatter("docs/gov/SL-GOV-001.md", self.doc(self.KEYS)), [])
        self.assertEqual(check.check_frontmatter("docs/gov/SL-GOV-001.md", self.doc(self.KEYS + ["max_lines"])), [])

    def test_missing_key_is_named(self):
        got = check.check_frontmatter("docs/gov/SL-GOV-001.md", self.doc([k for k in self.KEYS if k != "owner"]))
        self.assertIn("missing key 'owner'", got[0][3])

    def test_misordered_key_is_named(self):
        keys = list(self.KEYS)
        keys[0], keys[1] = keys[1], keys[0]
        got = check.check_frontmatter("docs/gov/SL-GOV-001.md", self.doc(keys))
        self.assertIn("'kind' is out of order", got[0][3])

    def test_no_front_matter(self):
        self.assertTrue(check.check_frontmatter("docs/gov/SL-GOV-001.md", "# T\n"))
        self.assertEqual(check.check_frontmatter(check.FRONT_EXEMPT[0], "# T\n"), [])


class Allowlist(unittest.TestCase):
    def test_rules(self):
        rules = [("python", "tools/docs/*.py"), ("*", "a/b.json")]
        self.assertTrue(check.allowed(rules, "python", "tools/docs/x.py"))
        self.assertFalse(check.allowed(rules, "shell", "tools/docs/x.py"))
        self.assertTrue(check.allowed(rules, "json", "a/b.json"))


if __name__ == "__main__":
    unittest.main()
