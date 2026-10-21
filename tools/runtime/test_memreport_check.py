"""Tests for runtime-memreport-check (WO-085, RT-INV-DIAG-06): the schema example passes and each planted bad report
is rejected with a finding that names the field. The example is schemas/runtime/examples/memreport.json when it
exists, else an inline copy. No subprocess: main() runs in-process against a temp root.
"""

import copy
import importlib.util
import io
import json
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("runtime_memreport_check", os.path.join(_here, "memreport_check.py"))
mc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mc)

REPO = os.path.dirname(os.path.dirname(_here))
EXAMPLE_PATH = os.path.join(REPO, "schemas", "runtime", "examples", "memreport.json")
INLINE_EXAMPLE = {
    "format": "sl-runtime-memreport/1",
    "instance": 1,
    "frame": 600,
    "tags": [
        {"tag": "ecs.chunks", "live_bytes": 4194304, "live_blocks": 2, "peak_bytes": 4194304, "allocs": 2,
         "frees": 0, "failures": 0},
        {"tag": "frame.arena", "live_bytes": 10485760, "live_blocks": 10, "peak_bytes": 10485760, "allocs": 10,
         "frees": 0, "failures": 0},
    ],
    "allocators": [
        {"name": "ecs.chunk_pool", "free_bytes": 2015232, "largest_free_run": 1966080, "fragmentation_permille": 25},
    ],
    "external": [{"tag": "fabric.projection", "bytes": 40960}],
    "leaks": [],
    "upstream_allocs_last_frame": 0,
}


def example():
    if os.path.isfile(EXAMPLE_PATH):
        with open(EXAMPLE_PATH, encoding="utf-8") as fh:
            return json.load(fh)
    return copy.deepcopy(INLINE_EXAMPLE)


def with_leaks(report, leaks):
    r = copy.deepcopy(report)
    r["leaks"] = leaks
    return r


class MemreportCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = mc.load_schema(REPO)

    def findings(self, report, no_leaks=False):
        return mc.check_report(report, self.schema, no_leaks)

    def assert_rejected(self, report, field, no_leaks=False):
        found = self.findings(report, no_leaks)
        self.assertTrue(found, "planted report passed")
        text = "\n".join("%s: %s" % f for f in found)
        self.assertIn(field, text)

    def test_rt_inv_diag_06_accepts_the_schema_example(self):
        self.assertEqual(self.findings(example()), [])
        self.assertEqual(self.findings(INLINE_EXAMPLE), [])
        self.assertEqual(self.findings(example(), no_leaks=True), [])

    def test_rt_inv_diag_06_rejects_schema_violations_naming_the_field(self):
        cases = []
        r = example()
        del r["frame"]
        cases.append((r, "frame"))
        r = example()
        r["format"] = "sl-runtime-memreport/2"
        cases.append((r, "/format"))
        r = example()
        r["instance"] = 0
        cases.append((r, "/instance"))
        r = example()
        r["extra"] = 1
        cases.append((r, "/extra"))
        r = example()
        r["tags"][0]["allocs"] = "2"
        cases.append((r, "/tags/0/allocs"))
        r = example()
        r["tags"][1]["frees"] = -1
        cases.append((r, "/tags/1/frees"))
        r = example()
        r["tags"][0]["peak_bytes"] = 1.5
        cases.append((r, "/tags/0/peak_bytes"))
        r = example()
        r["tags"][0]["tag"] = "Bad Tag"
        cases.append((r, "/tags/0/tag"))
        r = example()
        r["allocators"][0]["fragmentation_permille"] = 1001
        cases.append((r, "/allocators/0/fragmentation_permille"))
        r = example()
        r["external"][0]["tag"] = "ecs.chunks"
        cases.append((r, "/external/0/tag"))
        r = example()
        del r["external"][0]["bytes"]
        cases.append((r, "bytes"))
        r = with_leaks(example(), [{"tag": "events", "site": "a.cpp:1", "size": 0}])
        cases.append((r, "/leaks/0/size"))
        r = with_leaks(example(), [{"tag": "events", "site": "", "size": 8}])
        cases.append((r, "/leaks/0/site"))
        for report, field in cases:
            with self.subTest(field=field):
                self.assert_rejected(report, field)

    def test_rt_inv_diag_06_rejects_semantic_violations_naming_the_field(self):
        r = example()
        r["tags"][0]["live_bytes"] = r["tags"][0]["peak_bytes"] + 1
        self.assert_rejected(r, "/tags/0/live_bytes")
        r = example()
        r["tags"][1]["frees"] = r["tags"][1]["allocs"] + 1
        self.assert_rejected(r, "/tags/1/frees")
        r = example()
        r["tags"].reverse()
        self.assert_rejected(r, "/tags/1/tag")
        r = example()
        r["tags"].append(copy.deepcopy(r["tags"][-1]))
        self.assert_rejected(r, "/tags/2/tag")
        sorted_leaks = [
            {"tag": "ecs.chunks", "site": "a.cpp:1", "size": 8},
            {"tag": "ecs.chunks", "site": "a.cpp:1", "size": 16},
            {"tag": "events", "site": "a.cpp:1", "size": 4},
        ]
        self.assertEqual(self.findings(with_leaks(example(), sorted_leaks)), [])
        unsorted = [sorted_leaks[1], sorted_leaks[0], sorted_leaks[2]]
        self.assert_rejected(with_leaks(example(), unsorted), "/leaks/1")

    def test_rt_inv_diag_06_no_leaks_fails_on_a_leak_naming_tag_and_site(self):
        planted = with_leaks(example(), [{"tag": "neg.leak", "site": "neg.cpp:1", "size": 64}])
        self.assertEqual(self.findings(planted), [])
        found = self.findings(planted, no_leaks=True)
        self.assertEqual(len(found), 1)
        pointer, message = found[0]
        self.assertEqual(pointer, "/leaks/0")
        self.assertIn("neg.leak", message)
        self.assertIn("neg.cpp:1", message)
        self.assertIn("64", message)

    def test_rt_inv_diag_06_main_reads_files_and_defaults(self):
        tmp = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(tmp, "schemas", "runtime"))
            shutil.copy(os.path.join(REPO, mc.SCHEMA_PATH), os.path.join(tmp, mc.SCHEMA_PATH))
            out = io.StringIO()
            self.assertEqual(mc.main([], root=tmp, out=out), 1)
            self.assertIn("just runtime-test diag", out.getvalue())

            test_dir = os.path.join(tmp, "build", "runtime", "test")
            os.makedirs(test_dir)
            good = os.path.join(test_dir, "memreport.json")
            with open(good, "w", encoding="utf-8") as fh:
                json.dump(example(), fh)
            out = io.StringIO()
            self.assertEqual(mc.main([], root=tmp, out=out), 0)
            self.assertIn("OK", out.getvalue())

            leaky = os.path.join(tmp, "leaky.json")
            with open(leaky, "w", encoding="utf-8") as fh:
                json.dump(with_leaks(example(), [{"tag": "neg.leak", "site": "neg.cpp:1", "size": 64}]), fh)
            out = io.StringIO()
            self.assertEqual(mc.main([leaky], root=tmp, out=out), 0)
            out = io.StringIO()
            self.assertEqual(mc.main([leaky, "--no-leaks"], root=tmp, out=out), 1)
            self.assertIn("neg.leak", out.getvalue())
            self.assertIn("neg.cpp:1", out.getvalue())

            broken = os.path.join(tmp, "broken.json")
            with open(broken, "w", encoding="utf-8") as fh:
                fh.write('{"format": ')
            out = io.StringIO()
            self.assertEqual(mc.main([broken], root=tmp, out=out), 1)
            self.assertEqual(mc.main(["--bogus"], root=tmp, out=io.StringIO()), 2)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
