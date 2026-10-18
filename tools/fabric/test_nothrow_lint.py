"""Tests for the fabric nothrow lint (WO-036): one planted file per rule under tests/governance/fabric/nothrow/."""

import contextlib
import importlib.util
import io
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("nothrow_lint", os.path.join(_here, "nothrow_lint.py"))
nl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nl)

ROOT = os.path.dirname(os.path.dirname(_here))
FIXTURES = os.path.join(ROOT, "tests", "governance", "fabric", "nothrow")
PLANTED = (
    ("n_throw.cpp", "N-THROW"),
    ("n_try.cpp", "N-TRY"),
    ("n_at.cpp", "N-AT"),
    ("n_sto.cpp", "N-STO"),
    ("n_fs.cpp", "N-FS"),
    ("n_unordered.cpp", "N-UNORDERED"),
    ("n_new.cpp", "N-NEW"),
)


def read(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as fh:
        return fh.read()


def plant_line(text):
    return [i + 1 for i, line in enumerate(text.splitlines()) if "PLANT" in line][0]


class NothrowLintTest(unittest.TestCase):
    def test_fab_inv_cf_04_rules(self):
        self.assertEqual(nl.lint_text(read("clean.cpp")), [], "comments, strings, = delete, error_code are clean")
        for name, rule in PLANTED:
            text = read(name)
            hits = nl.lint_text(text)
            self.assertIn((plant_line(text), rule), [(line, r) for line, r, _ in hits], name)
            self.assertEqual({r for _, r, _ in hits}, {rule}, "%s trips only %s" % (name, rule))

    def test_tree_reports_file_and_line(self):
        errors, files, skipped = nl.lint_tree(FIXTURES, dirs=(".",), allow_path=os.devnull)
        self.assertEqual((files, skipped), (8, 0))
        self.assertIn("n_throw.cpp:%d: N-THROW" % plant_line(read("n_throw.cpp")), "\n".join(errors))

    def test_allow_list_needs_a_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            allow = os.path.join(tmp, "allow.txt")
            with open(allow, "w", encoding="utf-8") as fh:
                fh.write("# comment\nn_throw.cpp:N-THROW audited: rethrown at the C boundary\nn_try.cpp:N-TRY\n")
                fh.write("n_at.cpp:N-BOGUS because\n")
            errors, _files, skipped = nl.lint_tree(FIXTURES, dirs=(".",), allow_path=allow)
            text = "\n".join(errors)
            self.assertEqual(skipped, 1)
            self.assertNotIn("n_throw.cpp:", text)
            self.assertIn("allow.txt:3: N-ALLOW: entry n_try.cpp:N-TRY has no reason", text)
            self.assertIn("allow.txt:4: N-ALLOW", text)
            self.assertIn("n_try.cpp:2: N-TRY", text, "an entry without a reason allows nothing")

    def test_skips_testing_and_tools_and_missing_dirs(self):
        with tempfile.TemporaryDirectory() as tmp:
            for sub in ("src", "src/testing", "src/tools"):
                os.makedirs(os.path.join(tmp, "engine", "fabric", *sub.split("/")))
                shutil.copy(os.path.join(FIXTURES, "n_throw.cpp"), os.path.join(tmp, "engine", "fabric", sub))
            errors, files, _ = nl.lint_tree(tmp, allow_path=os.devnull)
            self.assertEqual(files, 1)
            self.assertEqual(len(errors), 1)
            self.assertIn("engine/fabric/src/n_throw.cpp:3: N-THROW", errors[0])
            empty, none, _ = nl.lint_tree(os.path.join(tmp, "nothing"), allow_path=os.devnull)
            self.assertEqual((empty, none), ([], 0))

    def test_main_exit_codes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            bad = nl.main(["x", "--root", FIXTURES, "--dirs", ".", "--allow", os.devnull])
        self.assertEqual(bad, 1)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(nl.main(["x", "--nope"]), 2)
        self.assertIn("fabric-nothrow-check: FAIL", out.getvalue())


if __name__ == "__main__":
    unittest.main()
