"""The fabric-determinism comparator (WO-041): identical outputs equal to accept.json pass; a compiler difference, a
corpus difference and a malformed file each fail naming the rule and the first differing vector."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import determinism  # noqa: E402

ACCEPT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tests", "fixtures", "fabric", "slb", "accept.json")


class ComparatorTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.clang = os.path.join(self.tmp.name, "corpus-clang.json")
        self.gcc = os.path.join(self.tmp.name, "corpus-gcc.json")
        text = determinism.synthesize(ACCEPT)
        for path in (self.clang, self.gcc):
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)

    def edit(self, path, name, key, value):
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
        row = next(r for r in doc["vectors"] if r["name"] == name)
        row[key] = value
        body = ",\n".join(json.dumps(r, ensure_ascii=False) for r in doc["vectors"])
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{\n"format": "%s",\n"vectors": [\n%s\n]\n}\n' % (doc["format"], body))

    def run_main(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = determinism.main(["determinism.py", self.clang, self.gcc, ACCEPT])
        return status, out.getvalue()

    def test_identical_outputs_equal_to_the_corpus_pass(self):
        status, out = self.run_main()
        self.assertEqual(status, 0, out)
        self.assertIn("byte-identical", out)

    def test_a_compiler_difference_names_the_vector(self):
        self.edit(self.gcc, "uint_300", "hex", "03ac03")
        status, out = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("R-DET-COMPILER", out)
        self.assertIn("'uint_300'", out)

    def test_a_corpus_difference_names_the_vector(self):
        for path in (self.clang, self.gcc):
            self.edit(path, "f64_one_and_half", "text", "1.50")
        status, out = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("R-DET-CORPUS", out)
        self.assertIn("'f64_one_and_half'", out)

    def test_a_malformed_output_is_a_format_error(self):
        with open(self.gcc, "w", encoding="utf-8") as handle:
            handle.write("{}")
        with self.assertRaises(determinism.DeterminismError) as caught:
            determinism.compare(self.clang, self.gcc, ACCEPT)
        self.assertEqual(caught.exception.rule, "R-DET-FORMAT")

    def test_a_missing_vector_is_named(self):
        for path in (self.clang, self.gcc):
            with open(path, encoding="utf-8") as handle:
                doc = json.load(handle)
            doc["vectors"] = doc["vectors"][:-1]
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(doc, handle)
        last = determinism.load_accept(ACCEPT)[-1]["name"]
        with self.assertRaises(determinism.DeterminismError) as caught:
            determinism.compare(self.clang, self.gcc, ACCEPT)
        self.assertEqual(caught.exception.rule, "R-DET-CORPUS")
        self.assertIn("'%s'" % last, str(caught.exception))


if __name__ == "__main__":
    unittest.main()
