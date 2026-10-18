"""Unit tests for tools/testing/fixtures.py (WO-027)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fixtures  # noqa: E402


class FixturesTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self._tmp.name, "fx")
        os.makedirs(os.path.join(self.dir, "sub"))
        self.write("a.txt", "alpha\n")
        self.write("sub/b.json", "{}\n")

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, text):
        with open(os.path.join(self.dir, rel), "w", encoding="utf-8") as handle:
            handle.write(text)

    def rules(self):
        return sorted(line.split()[0] for line in fixtures.check(self.dir))

    def test_no_manifest(self):
        self.assertEqual(self.rules(), ["R-FIX-NOMANIFEST"])

    def test_lock_then_check_is_clean(self):
        self.assertEqual(fixtures.lock(self.dir), [])
        self.assertEqual(self.rules(), [])

    def test_manifest_excludes_itself(self):
        fixtures.lock(self.dir)
        fixtures.lock(self.dir)
        files, _ = fixtures.scan(self.dir)
        self.assertEqual(sorted(files), ["a.txt", "sub/b.json"])
        self.assertEqual(self.rules(), [])

    def test_drift(self):
        fixtures.lock(self.dir)
        self.write("a.txt", "alpha!\n")
        self.assertEqual(self.rules(), ["R-FIX-DRIFT"])

    def test_unlisted(self):
        fixtures.lock(self.dir)
        self.write("new.txt", "x\n")
        self.assertEqual(self.rules(), ["R-FIX-UNLISTED"])

    def test_missing(self):
        fixtures.lock(self.dir)
        os.remove(os.path.join(self.dir, "a.txt"))
        self.assertEqual(self.rules(), ["R-FIX-MISSING"])

    def test_schema(self):
        self.write("MANIFEST.json", "{\"schema\": \"nope\"}\n")
        self.assertEqual(self.rules(), ["R-FIX-SCHEMA"])
        self.write("MANIFEST.json", "not json")
        self.assertEqual(self.rules(), ["R-FIX-SCHEMA"])

    def test_symlink(self):
        fixtures.lock(self.dir)
        os.symlink("a.txt", os.path.join(self.dir, "link"))
        self.assertIn("R-FIX-SYMLINK", self.rules())


if __name__ == "__main__":
    unittest.main()
