"""Drift detection of the SLB/1 golden corpus (WO-038): the committed files equal the generator, and a planted
change is reported with its file and vector name."""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus  # noqa: E402


def quiet(fn, *args):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args)


class CommittedCorpusTest(unittest.TestCase):
    def test_committed_corpus_matches_the_generator(self):
        self.assertEqual(quiet(corpus.check, corpus.DEFAULT_DIR), [])

    def test_sizes_and_unique_names(self):
        accept, reject = corpus.accept_vectors(), corpus.reject_vectors()
        self.assertGreaterEqual(len(accept), corpus.MIN_ACCEPT)
        self.assertGreaterEqual(len(reject), corpus.MIN_REJECT)
        for vectors in (accept, reject):
            names = [v["name"] for v in vectors]
            self.assertEqual(len(names), len(set(names)))

    def test_generation_is_deterministic(self):
        self.assertEqual(corpus.render_all(), corpus.render_all())

    def test_varint_float_and_depth_boundaries_are_present(self):
        names = {v["name"] for v in corpus.accept_vectors()}
        for want in (
            "uint_0",
            "uint_127",
            "uint_128",
            "uint_9223372036854775808",
            "uint_18446744073709551615",
            "nint_m1",
            "nint_m9223372036854775808",
            "f64_min_subnormal",
            "f64_max",
            "f32_min_subnormal",
            "f64_neg_zero_input",
            "str_empty",
            "str_four_byte",
            "depth_list_64",
            "oset_ties_by_element_id",
            "unknown_fields_scalar",
            "map_int_keys_mixed",
        ):
            self.assertIn(want, names)
        rejects = {v["name"] for v in corpus.reject_vectors()}
        for want in ("tag_00_root", "tag_10_root", "limit_depth_65_lists", "frame_major_02", "trunc_user_00"):
            self.assertIn(want, rejects)


class PlantedDriftTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self._tmp.name, "slb")
        shutil.copytree(corpus.DEFAULT_DIR, self.dir)

    def tearDown(self):
        self._tmp.cleanup()

    def edit(self, name, fn):
        path = os.path.join(self.dir, name)
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
        fn(doc)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(corpus.render_vectors(doc["format"], doc["vectors"]))

    def findings(self):
        return quiet(corpus.check, self.dir)

    def test_control_is_clean(self):
        self.assertEqual(self.findings(), [])

    def test_flipped_hex_digit_names_the_file_and_the_vector(self):
        def flip(doc):
            v = next(x for x in doc["vectors"] if x["name"] == "uint_300")
            v["hex"] = v["hex"][:-1] + ("0" if v["hex"][-1] != "0" else "1")

        self.edit("accept.json", flip)
        out = "\n".join(self.findings())
        self.assertIn("R-GOLD-HEX accept.json: vector 'uint_300'", out)
        self.assertIn("R-GOLD-DRIFT accept.json: vector 'uint_300'", out)

    def test_changed_reject_offset_is_named(self):
        def bump(doc):
            next(x for x in doc["vectors"] if x["name"] == "tag_00_in_list")["offset"] += 1

        self.edit("reject.json", bump)
        self.assertIn("R-GOLD-REJECT reject.json: vector 'tag_00_in_list'", "\n".join(self.findings()))

    def test_changed_reject_rule_is_named(self):
        def rule(doc):
            next(x for x in doc["vectors"] if x["name"] == "nan_f64_quiet_nan")["rule"] = "R-SER-NEGZERO"

        self.edit("reject.json", rule)
        self.assertIn("vector 'nan_f64_quiet_nan'", "\n".join(self.findings()))

    def test_changed_text_is_named(self):
        def text(doc):
            next(x for x in doc["vectors"] if x["name"] == "f64_one_and_half")["text"] = "1.50"

        self.edit("accept.json", text)
        self.assertIn("R-GOLD-TEXT accept.json: vector 'f64_one_and_half'", "\n".join(self.findings()))

    def test_removed_vector_is_reported(self):
        def drop(doc):
            doc["vectors"] = [x for x in doc["vectors"] if x["name"] != "bool_true"]

        self.edit("accept.json", drop)
        self.assertIn("R-GOLD-DRIFT accept.json: vector 'bool_true': missing from the file", "\n".join(self.findings()))

    def test_added_vector_is_reported(self):
        def add(doc):
            extra = dict(doc["vectors"][0])
            extra["name"] = "extra_vector"
            doc["vectors"].append(extra)

        self.edit("accept.json", add)
        self.assertIn("extra_vector", "\n".join(self.findings()))

    def test_coverage_is_enforced(self):
        def shrink(doc):
            doc["vectors"] = [x for x in doc["vectors"] if x["rule"] != "R-SER-FRAME"]

        self.edit("reject.json", shrink)
        out = "\n".join(self.findings())
        self.assertIn("R-GOLD-COVERAGE reject.json: no must-reject vector for R-SER-FRAME", out)

    def test_missing_file_is_reported(self):
        os.remove(os.path.join(self.dir, "reject.json"))
        self.assertTrue(self.findings()[0].startswith("R-GOLD-NOFILE reject.json"))

    def test_schema_drift_is_reported(self):
        path = os.path.join(self.dir, "schemas.json")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text.replace('"bits": 8', '"bits": 7'))
        self.assertTrue(any(f.startswith("R-GOLD-") and "schemas.json" in f for f in self.findings()))

    def test_write_then_check_round_trips(self):
        other = os.path.join(self._tmp.name, "fresh")
        corpus.write(other)
        self.assertEqual(quiet(corpus.check, other), [])
        for name in corpus.FILES:
            with open(os.path.join(other, name), "rb") as a, open(os.path.join(corpus.DEFAULT_DIR, name), "rb") as b:
                self.assertEqual(a.read(), b.read())


if __name__ == "__main__":
    unittest.main()
