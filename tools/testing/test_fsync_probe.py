"""Unit tests for tools/testing/fsync_probe.py (WO-065)."""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsync_probe  # noqa: E402


class FsyncProbeTest(unittest.TestCase):
    def test_sample_count_and_shape(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples, summary = fsync_probe.probe(tmp, files=3, size=1024)
            self.assertEqual(len(samples), 3)
            for sample in samples:
                self.assertEqual(sorted(sample), ["bytes", "dir_fsync_ms", "file", "fsync_ms"])
                self.assertEqual(sample["bytes"], 1024)
                self.assertGreaterEqual(sample["fsync_ms"], 0)
                self.assertGreaterEqual(sample["dir_fsync_ms"], 0)
            self.assertEqual(sorted(summary), ["max_ms", "median_ms", "min_ms", "samples"])
            self.assertEqual(summary["samples"], 3)
            self.assertLessEqual(summary["min_ms"], summary["median_ms"])
            self.assertLessEqual(summary["median_ms"], summary["max_ms"])
            self.assertEqual(os.listdir(tmp), [])

    def test_main_exits_zero(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            with tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(fsync_probe.main([tmp, "--files", "2", "--bytes", "512"]), 0)
            self.assertEqual(fsync_probe.main([]), 0)
            self.assertEqual(fsync_probe.main([os.path.join(tmp, "missing")]), 0)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn('"summary"', lines[-1])


if __name__ == "__main__":
    unittest.main()
