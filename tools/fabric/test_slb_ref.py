"""Unit tests of the SLB/1 reference codec (WO-038). FAB-INV-SER-01: the committed corpus."""

import hashlib
import json
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
from slb_ref import (  # noqa: E402
    F32,
    F64,
    Bytes,
    Digest,
    Id,
    List,
    Map,
    Nint,
    Oset,
    SlbError,
    Str,
    Struct,
    Uint,
    Variant,
)

CORPUS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                      "tests", "fixtures", "fabric", "slb")


def load(name):
    with open(os.path.join(CORPUS, name), encoding="utf-8") as handle:
        return json.load(handle)


def rejection(fn, *args, **kw):
    try:
        fn(*args, **kw)
    except SlbError as err:
        return err.rule, err.offset
    return None


class CorpusTest(unittest.TestCase):
    def test_fab_inv_ser_01_corpus(self):
        accept = load("accept.json")["vectors"]
        reject = load("reject.json")["vectors"]
        schemas = load("schemas.json")["types"]
        self.assertGreaterEqual(len(accept), 150)
        self.assertGreaterEqual(len(reject), 60)
        for v in accept:
            value = slb_ref.value_from_json(v["value"])
            payload = slb_ref.encode(value)
            self.assertEqual(payload.hex(), v["hex"], v["name"])
            self.assertEqual(slb_ref.oid_text(slb_ref.oid(2, payload)), v["oid"], v["name"])
            self.assertEqual(slb_ref.encode(slb_ref.decode(payload)), payload, v["name"])
            if "unknown" not in v["name"]:
                slb_ref.decode(payload, "strict", schemas, v["type"])
        for v in reject:
            data = bytes.fromhex(v["hex"])
            if v["target"] == "frame":
                got = rejection(slb_ref.parse_frame, data)
            else:
                got = rejection(slb_ref.decode, data, v["policy"], schemas, v["type"])
            self.assertEqual(got, (v["rule"], v["offset"]), v["name"])

    def test_every_rule_has_a_reject_vector(self):
        rules = {v["rule"] for v in load("reject.json")["vectors"]}
        self.assertEqual(rules, set(slb_ref.RULES))

    def test_unknown_fields_survive_preserve_and_fail_strict(self):
        schemas = load("schemas.json")["types"]
        data = slb_ref.encode(Struct(((1, Uint(5)), (7, Uint(1)))))
        self.assertEqual(slb_ref.encode(slb_ref.decode(data, "preserve", schemas, "rec_ab")), data)
        self.assertEqual(rejection(slb_ref.decode, data, "strict", schemas, "rec_ab"), ("R-SER-UNKNOWN", 5))
        self.assertEqual(slb_ref.encode(slb_ref.decode(data, "strict")), data)  # no schema: nothing is unknown
        with self.assertRaises(ValueError):
            slb_ref.decode(data, "lenient")


class HandComputedTest(unittest.TestCase):
    """Bytes worked out by hand from ADR-0012, independent of the corpus generator."""

    def test_scalars(self):
        cases = [
            (False, "01"),
            (True, "02"),
            (Uint(0), "0300"),
            (Uint(127), "037f"),
            (Uint(128), "038001"),
            (Uint(300), "03ac02"),
            (Uint(2**64 - 1), "03ffffffffffffffffff01"),
            (Nint(-1), "0400"),
            (Nint(-2), "0401"),
            (Nint(-129), "048001"),
            (Nint(-(2**63)), "04ffffffffffffffff7f"),
            (F64(1.5), "06000000000000f83f"),
            (F64(-0.0), "060000000000000000"),
            (F32(1.0), "050000803f"),
            (F32(-0.0), "0500000000"),
            (Str(""), "0700"),
            (Str("é"), "0702c3a9"),
            (Bytes(b"\x00\xff"), "080200ff"),
            (Id(bytes(range(16))), "09000102030405060708090a0b0c0d0e0f"),
            (Digest(1, b"\xaa" * 32), "0a01" + "aa" * 32),
        ]
        for value, want in cases:
            self.assertEqual(slb_ref.encode(value).hex(), want, repr(value))

    def test_containers_sort_by_encoded_bytes(self):
        self.assertEqual(slb_ref.encode(List((Uint(1), False))).hex(), "0b02030101")
        m = Map(((Str("aa"), False), (Str("b"), True)))
        self.assertEqual(slb_ref.encode(m).hex(), "0c02" + "07016202" + "0702616101")
        m = Map(((Uint(129), False), (Uint(16384), False)))  # 03 80 80 01 sorts before 03 81 01
        self.assertEqual(slb_ref.encode(m).hex(), "0c02" + "03808001" + "01" + "038101" + "01")
        self.assertEqual(slb_ref.encode(Struct(((2, True), (1, False)))).hex(), "0d02" + "0101" + "0202")
        self.assertEqual(slb_ref.encode(Variant(128, Uint(1))).hex(), "0e8001" + "0301")

    def test_oset_sorts_by_order_key_then_element_id(self):
        a, b, c = (bytes([12, 1]) + b"\x00" * 13 + bytes([n]) for n in (1, 2, 3))
        v = Oset(((b, b"k", False), (a, b"k", True), (c, b"", False)))
        want = "0f03" + c.hex() + "00" + "01" + a.hex() + "016b" + "02" + b.hex() + "016b" + "01"
        self.assertEqual(slb_ref.encode(v).hex(), want)

    def test_frame_and_oid(self):
        self.assertEqual(slb_ref.frame(2, b"").hex(), "534c464f010002" + "00" + "00")
        pre = b"slf-obj/1\x00" + bytes([1, 0, 2, 0]) + b"\x00"
        self.assertEqual(slb_ref.oid(2, b"").hex(), "01" + hashlib.sha256(pre).hexdigest())
        f = slb_ref.parse_frame(slb_ref.frame(4, b"abc", minor=3))
        self.assertEqual((f.kind, f.minor, f.payload, f.read_only), (4, 3, b"abc", True))
        self.assertFalse(slb_ref.parse_frame(slb_ref.frame(4, b"abc")).read_only)

    def test_oid_covers_every_header_byte(self):
        base = slb_ref.oid(2, b"xyz")
        for variant in (
            slb_ref.oid(2, b"xyz", major=2),
            slb_ref.oid(2, b"xyz", minor=1),
            slb_ref.oid(3, b"xyz"),
            slb_ref.oid(2, b"xyz", flags=1),
            slb_ref.oid(2, b"xya"),
        ):
            self.assertNotEqual(base, variant)


class EncoderRefusalsTest(unittest.TestCase):
    def test_refusals(self):
        cases = [
            (Nint(5), "R-SER-ENCODE"),
            (Uint(2**64), "R-SER-ENCODE"),
            (Uint(-1), "R-SER-ENCODE"),
            (F64(float("nan")), "R-SER-NAN"),
            (F64(float("inf")), "R-SER-NAN"),
            (F32(-float("inf")), "R-SER-NAN"),
            (F32(0.1), "R-SER-ENCODE"),
            (F32(1e39), "R-SER-ENCODE"),
            (Str("a\x00b"), "R-SER-UTF8"),
            (Str("\ud800"), "R-SER-UTF8"),
            (Map(((Str("a"), False), (Str("a"), True))), "R-SER-DUP"),
            (Map(((True, False),)), "R-SER-TAG"),
            (Struct(((1, False), (1, True))), "R-SER-DUP"),
            (Struct(((2**32, False),)), "R-SER-LIMIT"),
            (Variant(2**32, False), "R-SER-LIMIT"),
            (Oset(((b"\x01" * 16, b"a", False), (b"\x01" * 16, b"b", False))), "R-SER-DUP"),
            (Id(b"\x00" * 15), "R-SER-ENCODE"),
            (Digest(1, b"\x00" * 31), "R-SER-ENCODE"),
            ("text", "R-SER-ENCODE"),
        ]
        for value, rule in cases:
            self.assertEqual(rejection(slb_ref.encode, value)[0], rule, repr(value))

    def test_depth_limit_on_encode(self):
        value = List(())
        for _ in range(63):
            value = List((value,))
        slb_ref.encode(value)  # 64 containers
        self.assertEqual(rejection(slb_ref.encode, List((value,)))[0], "R-SER-LIMIT")

    def test_negative_zero_is_normalised_in_containers(self):
        self.assertEqual(slb_ref.encode(List((F64(-0.0),))), slb_ref.encode(List((F64(0.0),))))
        packed = slb_ref.encode(F32(-0.0))
        self.assertEqual(struct.unpack("<f", packed[1:])[0], 0.0)
        self.assertEqual(packed[1:], b"\x00\x00\x00\x00")


class DecoderTest(unittest.TestCase):
    def test_varint_edges(self):
        self.assertEqual(slb_ref.decode(bytes.fromhex("03ffffffffffffffffff01")), Uint(2**64 - 1))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("03ffffffffffffffffff02")), ("R-SER-VARINT", 1))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("038000")), ("R-SER-VARINT", 1))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("0380")), ("R-SER-TRUNC", 2))

    def test_float_rules(self):
        self.assertEqual(slb_ref.decode(bytes.fromhex("06" + "0100000000000080")), F64(-5e-324))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("050000807f")), ("R-SER-NAN", 0))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("0500000080")), ("R-SER-NEGZERO", 0))

    def test_limits_precede_truncation(self):
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("0b81808008")), ("R-SER-LIMIT", 1))
        self.assertEqual(rejection(slb_ref.decode, bytes.fromhex("0b80808008")), ("R-SER-TRUNC", 5))
        self.assertEqual(rejection(slb_ref.decode, b"\x01" * (slb_ref.MAX_BODY + 1)), ("R-SER-LIMIT", 0))

    def test_first_violation_in_byte_order_wins(self):
        # an unsorted pair whose first key is also the first problem: order, not the later bad UTF-8
        data = bytes.fromhex("0c02" + "07016201" + "07016101" + "0701ff")
        self.assertEqual(rejection(slb_ref.decode, data), ("R-SER-ORDER", 6))

    def test_frame_checks_run_in_offset_order(self):
        self.assertEqual(rejection(slb_ref.parse_frame, bytes.fromhex("534c464f02ff0201")), ("R-SER-FRAME", 4))
        self.assertEqual(rejection(slb_ref.parse_frame, bytes.fromhex("534c464f01000b01")), ("R-SER-FRAME", 6))
        self.assertEqual(rejection(slb_ref.parse_frame, bytes.fromhex("534c464f01000201")), ("R-SER-FRAME", 7))


if __name__ == "__main__":
    unittest.main()
