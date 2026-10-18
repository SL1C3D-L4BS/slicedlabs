"""Unit tests of the canonical text projection (WO-038). FAB-INV-SER-02: binary and text are bijective."""

import json
import os
import random
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
import slb_text  # noqa: E402
from slb_ref import F32, F64, SlbError  # noqa: E402

CORPUS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                      "tests", "fixtures", "fabric", "slb")


def load(name):
    with open(os.path.join(CORPUS, name), encoding="utf-8") as handle:
        return json.load(handle)


SCHEMAS = load("schemas.json")["types"]


def fails(rule, fn, *args):
    try:
        fn(*args)
    except SlbError as err:
        return err.rule == rule
    return False


class BijectionTest(unittest.TestCase):
    def test_fab_inv_ser_02_bijective(self):
        vectors = load("accept.json")["vectors"]
        self.assertGreaterEqual(len(vectors), 150)
        for v in vectors:
            payload = bytes.fromhex(v["hex"])
            value = slb_ref.decode(payload)
            text = slb_text.to_text(value, v["type"], SCHEMAS)
            self.assertEqual(text, v["text"], v["name"])
            back = slb_text.from_text(text, v["type"], SCHEMAS)
            self.assertEqual(slb_ref.encode(back), payload, v["name"])  # binary -> text -> binary
            self.assertEqual(slb_text.to_text(back, v["type"], SCHEMAS), text, v["name"])  # text -> binary -> text

    def test_schemas_are_well_formed(self):
        self.assertEqual(slb_text.check_schemas(SCHEMAS), [])

    def test_the_text_is_compact_json(self):
        for v in load("accept.json")["vectors"]:
            json.loads(v["text"])
            outside = re.sub(r'"(?:[^"\\]|\\.)*"', '""', v["text"])
            self.assertFalse(re.search(r"\s", outside), v["name"])


class FloatTextTest(unittest.TestCase):
    def test_f64_text_equals_python_repr(self):
        rng = random.Random(7)
        for _ in range(3000):
            x = struct.unpack("<d", rng.getrandbits(64).to_bytes(8, "little"))[0]
            if x != x or x in (float("inf"), float("-inf")):
                continue
            text = slb_text.float_to_text(x, 8)
            if x == 0.0:
                self.assertEqual(text, "0.0")
                continue
            self.assertEqual(text, repr(x))
            self.assertEqual(slb_text.float_from_token(text, 8), x)

    def test_f32_text_round_trips_and_is_shortest(self):
        rng = random.Random(11)
        for _ in range(3000):
            x = struct.unpack("<f", rng.getrandbits(32).to_bytes(4, "little"))[0]
            if x != x or x in (float("inf"), float("-inf")) or x == 0.0:
                continue
            text = slb_text.float_to_text(x, 4)
            self.assertEqual(slb_text.float_from_token(text, 4), x, text)
            digits = text.lstrip("-").split("e")[0].replace(".", "").lstrip("0").rstrip("0")
            if len(digits) > 1:  # one digit fewer must not round-trip
                shorter = "%.*e" % (len(digits) - 2, abs(x))
                self.assertNotEqual(slb_text.float_from_token(shorter, 4), abs(x), (text, shorter))

    def test_format_edges(self):
        cases = [
            (1.5, 8, "1.5"),
            (100.0, 8, "100.0"),
            (1e15, 8, "1000000000000000.0"),
            (1e16, 8, "1e+16"),
            (1e-4, 8, "0.0001"),
            (1e-5, 8, "1e-05"),
            (5e-324, 8, "5e-324"),
            (-2.5, 8, "-2.5"),
            (3.4028234663852886e38, 4, "3.4028235e+38"),
            (1.401298464324817e-45, 4, "1e-45"),
        ]
        for x, width, want in cases:
            self.assertEqual(slb_text.float_to_text(x, width), want)


class NonCanonicalTextTest(unittest.TestCase):
    def rejects(self, text, type_name):
        return fails("R-TXT-SYNTAX", slb_text.from_text, text, type_name, SCHEMAS)

    def test_other_spellings_are_refused(self):
        for text, type_name in [
            (" 1", "u64"),
            ("1 ", "u64"),
            ("01", "u64"),
            ('"1"', "u64"),
            ("9007199254740992", "u64"),
            ('"9007199254740991"', "u64"),
            ('"-5"', "i64"),
            ("-0", "i64"),
            ("1.50", "f64"),
            ("-0.0", "f64"),
            ("1e0", "f64"),
            ("1E+16", "f64"),
            ("0.30000000000000004000", "f64"),
            ('"00FF"', "bytes"),
            ('"0"', "bytes"),
            ('"ast_040460t3gf208h44rm2mb1e60s"', "id"),
            ('"SHA256:' + "00" * 32 + '"', "digest"),
            ('"\\u0041"', "string"),
            ('"\\/"', "string"),
            ('{"a": 5}', "rec_ab"),
            ('{"b":"x","a":5}', "rec_ab"),
            ('{"a":5,"a":5}', "rec_ab"),
            ("NaN", "f64"),
            ("Infinity", "f64"),
            ("[ 1]", "list_u64"),
            ("[[\"b\",1],[\"a\",2]]", "map_string_u64"),
        ]:
            self.assertTrue(self.rejects(text, type_name), (text, type_name))

    def test_type_errors(self):
        for text, type_name in [("256", "u8"), ("-1", "u64"), ('{"zzz":1}', "rec_ab"), ("{}", "rec_ab"),
                                ('{"purple":1}', "scalar"), ('"purple"', "color")]:
            self.assertTrue(fails("R-TXT-TYPE", slb_text.from_text, text, type_name, SCHEMAS), (text, type_name))

    def test_to_text_type_errors(self):
        self.assertTrue(fails("R-TXT-TYPE", slb_text.to_text, slb_ref.Uint(256), "u8", SCHEMAS))
        self.assertTrue(fails("R-TXT-TYPE", slb_text.to_text, slb_ref.Str("x"), "u64", SCHEMAS))
        self.assertTrue(fails("R-TXT-TYPE", slb_text.to_text, slb_ref.Struct(()), "rec_ab", SCHEMAS))
        self.assertTrue(fails("R-TXT-TYPE", slb_text.to_text, slb_ref.Uint(9), "color", SCHEMAS))
        self.assertTrue(fails("R-TXT-TYPE", slb_text.to_text, slb_ref.Digest(9, b"\x00" * 32), "digest", SCHEMAS))

    def test_integer_text_switches_at_2p53(self):
        self.assertEqual(slb_text.to_text(slb_ref.Uint(2**53 - 1), "u64", SCHEMAS), "9007199254740991")
        self.assertEqual(slb_text.to_text(slb_ref.Uint(2**53), "u64", SCHEMAS), '"9007199254740992"')
        self.assertEqual(slb_text.to_text(slb_ref.Nint(-(2**53) + 1), "i64", SCHEMAS), "-9007199254740991")
        self.assertEqual(slb_text.to_text(slb_ref.Nint(-(2**53)), "i64", SCHEMAS), '"-9007199254740992"')

    def test_unknown_field_text(self):
        value = slb_ref.Struct(((1, slb_ref.Uint(5)), (7, slb_ref.Uint(1))))
        text = slb_text.to_text(value, "rec_ab", SCHEMAS)
        self.assertEqual(text, '{"a":5,"?7":"slb:0301"}')
        self.assertEqual(slb_text.from_text(text, "rec_ab", SCHEMAS), value)
        self.assertTrue(fails("R-TXT-SYNTAX", slb_text.from_text, '{"a":5,"?7":"0301"}', "rec_ab", SCHEMAS))


class IdTextTest(unittest.TestCase):
    def test_round_trip_all_classes(self):
        for cls in range(1, 16):
            raw = bytes([cls, 2]) + bytes(range(14))
            self.assertEqual(slb_text.id_from_text(slb_text.id_to_text(raw)), raw)

    def test_bad_ids(self):
        good = slb_text.id_to_text(bytes([4, 1]) + bytes(range(14)))
        bads = ("prj_" + good[4:], good[:-1], good + "0", good.lower(), good[:-1] + "U", "ast_8" + good[5:], "", "ast")
        for bad in bads:
            self.assertTrue(fails("R-TXT-SYNTAX", slb_text.id_from_text, bad), bad)
        self.assertTrue(fails("R-TXT-TYPE", slb_text.id_to_text, bytes([0, 1]) + bytes(14)))
        self.assertTrue(fails("R-TXT-TYPE", slb_text.id_to_text, bytes([16, 1]) + bytes(14)))


if __name__ == "__main__":
    unittest.main()
