"""Unit tests for constitution-frozen (temp-root fixtures)."""

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_constitution_check as cc  # noqa: E402
import cmd_constitution_frozen as cf  # noqa: E402
import slgov  # noqa: E402


def read(rel):
    with open(os.path.join(slgov.ROOT, *rel.split("/")), "rb") as fh:
        return fh.read()


PLAN = read(cc.PLAN_REL)
DOC = read(cc.CONSTITUTION_REL)


class FrozenTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write(self, rel, data):
        path = os.path.join(self.tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    def fixture(self, doc=DOC, approvals=None):
        self.write(cc.PLAN_REL, PLAN)
        self.write(cc.CONSTITUTION_REL, doc)
        if approvals is not None:
            self.write("registry/SL-REG-001.approvals.json", json.dumps(approvals).encode())

    def run_main(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cf.main(["x", "--root", self.tmp])
        return code, out.getvalue()

    def approved_doc(self):
        return DOC.replace(b"status: Draft", b"status: Approved", 1).replace(b"version: 0.1.0", b"version: 1.0.0", 1)

    def test_not_approved_exits_0(self):
        self.fixture(approvals={})
        code, out = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("not yet approved (episode 020 pending)", out)

    def test_missing_approvals_file_exits_0(self):
        self.fixture()
        self.assertEqual(self.run_main()[0], 0)

    def test_approved_passes(self):
        doc = self.approved_doc()
        self.fixture(doc, {"SL-GOV-001": {"sha256": hashlib.sha256(doc).hexdigest(), "version": "1.0.0"}})
        code, out = self.run_main()
        self.assertEqual((code, out), (0, "constitution-frozen: ok\n"))

    def test_wrong_sha(self):
        doc = self.approved_doc()
        self.fixture(doc, {"SL-GOV-001": {"sha256": "0" * 64, "version": "1.0.0"}})
        code, out = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("R-FROZEN-SHA", out)

    def test_draft_status(self):
        self.fixture(DOC, {"SL-GOV-001": {"sha256": hashlib.sha256(DOC).hexdigest(), "version": "0.1.0"}})
        code, out = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("R-FROZEN-STATUS", out)
        self.assertNotIn("R-FROZEN-SHA", out)

    def test_locked_row_changed(self):
        doc = self.approved_doc().replace(b"CMake + Ninja", b"CMake + Make", 1)
        self.assertNotEqual(doc, self.approved_doc())
        self.fixture(doc, {"SL-GOV-001": {"sha256": hashlib.sha256(doc).hexdigest(), "version": "1.0.0"}})
        code, out = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("R-FROZEN-LOCKED", out)

    def test_bad_entry_shape(self):
        self.fixture(DOC, {"SL-GOV-001": {"sha256": "xyz"}})
        code, out = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("R-FROZEN-SHA", out)

    def test_real_repo_passes_today(self):
        self.assertEqual(cf.frozen_errors(slgov.ROOT)[1], [])


if __name__ == "__main__":
    unittest.main()
