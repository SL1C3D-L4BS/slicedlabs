"""Unit tests for cmd_registry_schema_selftest.py: every schemas/sl-*.schema.json has an instance (WO-073)."""

import contextlib
import copy
import glob
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_registry_schema_selftest as st  # noqa: E402
import slib  # noqa: E402


class CoverageTests(unittest.TestCase):
    def test_every_schema_file_is_listed(self):
        paths = glob.glob(os.path.join(slib.SCHEMA_DIR, "sl-*.schema.json"))
        files = sorted(os.path.basename(p)[: -len(".schema.json")] for p in paths)
        self.assertEqual(files, sorted(st.SCHEMAS))

    def test_each_later_schema_has_an_instance(self):
        later = ("sl-fabric-event", "sl-log-event", "sl-provenance", "sl-deps", "sl-deps-lock")
        covered = set(st.INSTANCES) | {name for name, _ in st.REPO_INSTANCES}
        self.assertEqual(sorted(covered), sorted(later))

    def test_instances_validate_and_a_planted_defect_does_not(self):
        for name, instance in st.INSTANCES.items():
            schema = slib.load_schema(name)
            self.assertEqual(slib.validate(instance, schema), [], name)
            broken = copy.deepcopy(instance)
            broken["x_extra"] = 1
            self.assertNotEqual(slib.validate(broken, schema), [], name)
        for name, rel in st.REPO_INSTANCES:
            instance = slib.load_json(os.path.join(slib.ROOT, rel))
            self.assertEqual(slib.validate(instance, slib.load_schema(name)), [], name)

    def test_selftest_passes_and_reports_the_schema_count(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = st.main()
        self.assertEqual(status, 0, out.getvalue())
        self.assertIn("(%d schemas," % len(st.SCHEMAS), out.getvalue())


if __name__ == "__main__":
    unittest.main()
