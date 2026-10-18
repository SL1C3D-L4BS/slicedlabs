#!/usr/bin/env python3
"""SLB/1 golden corpus (WO-038, ARCH1-R26..R31, ADR-0012). Stdlib only, no subprocess, no network (ADR-0004).

Run with `python3 -I tools/fabric/corpus.py check|write [--dir DIR]` (default DIR tests/fixtures/fabric/slb).

  check   regenerates the corpus in memory with tools/fabric/slb_ref.py and slb_text.py and compares it with
          schemas.json, accept.json and reject.json in DIR; exits 1 and prints one finding per line
          (`RULE file: vector 'name': message`) otherwise. This is the gate behind `just fabric-golden-check`.
  write   regenerates the three files (the founder or a builder after a deliberate format change; never a gate).

Files (all UTF-8 JSON, one vector per line):
  schemas.json  {"format": "slb-corpus-schemas/1", "types": {...}}   the schemas that direct the text projection
  accept.json   {"format": "slb-accept/1", "vectors": [{name, type, value, hex, oid, text}, ...]}
                value: the notation of slb_ref.value_to_json; hex: the canonical SLB/1 bytes; oid: sha256 object
                id of the record frame (kind 2, major 1, minor 0, flags 0) around hex; text: canonical text
  reject.json   {"format": "slb-reject/1", "vectors": [{name, hex, rule, offset, target, policy, type}, ...]}
                target: "value" (decode a payload) or "frame" (parse a frame); policy: preserve or strict;
                type: schema type name for strict policy (else null); rule and offset as in slb_ref.py

Findings: R-GOLD-NOFILE, R-GOLD-SHAPE, R-GOLD-SCHEMA, R-GOLD-HEX, R-GOLD-OID, R-GOLD-DECODE, R-GOLD-TEXT,
R-GOLD-REJECT, R-GOLD-COVERAGE, R-GOLD-DRIFT.
"""

import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
import slb_text  # noqa: E402
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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIR = os.path.join(ROOT, "tests", "fixtures", "fabric", "slb")
MIN_ACCEPT = 150
MIN_REJECT = 60
RECORD_KIND = 2
FILES = ("schemas.json", "accept.json", "reject.json")


# ---- schemas ------------------------------------------------------------------------------------------------

def _t(kind, **kw):
    kw["kind"] = kind
    return kw


def _fields(*rows):
    out = []
    for row in rows:
        no, name, type_ref = row[:3]
        field = {"no": no, "name": name, "type": type_ref}
        if len(row) > 3 and row[3] is not None:
            field["optional"] = True
            if row[3] is not True:
                field["default"] = row[3]
        out.append(field)
    return out


def _cases(*rows):
    return [{"no": no, "name": name, "type": type_ref} for no, name, type_ref in rows]


SCHEMAS = {
    "bool": _t("bool"),
    "u8": _t("uint", bits=8),
    "u64": _t("uint"),
    "i64": _t("int"),
    "i65": _t("int", bits=65),
    "f32": _t("f32"),
    "f64": _t("f64"),
    "string": _t("string"),
    "bytes": _t("bytes"),
    "id": _t("id"),
    "digest": _t("digest"),
    "color": _t("enum", enumerators=[{"no": 0, "name": "red"}, {"no": 1, "name": "green"}, {"no": 2, "name": "blue"}]),
    "list_u64": _t("list", element="u64"),
    "list_string": _t("list", element="string"),
    "list_list_u64": _t("list", element="list_u64"),
    "list_scalar": _t("list", element="scalar"),
    "map_string_u64": _t("map", key="string", value="u64"),
    "map_u64_string": _t("map", key="uint", value="string"),
    "map_i64_u64": _t("map", key="int", value="u64"),
    "map_id_string": _t("map", key="id", value="string"),
    "map_string_point": _t("map", key="string", value="point"),
    "empty": _t("struct", fields=[]),
    "point": _t("struct", fields=_fields((1, "x", "f64"), (2, "y", "f64"))),
    "rec_ab": _t("struct", fields=_fields((1, "a", "u64"), (2, "b", "string", True))),
    "cfg": _t("struct", fields=_fields((1, "retries", "u64", 3), (2, "name", "string", ""))),
    "wide": _t(
        "struct",
        fields=_fields(
            (0, "f0", "u64"),
            (127, "f127", "u64"),
            (128, "f128", "u64"),
            (16384, "f16384", "u64"),
            (4294967295, "fmax", "u64"),
        ),
    ),
    "user": _t(
        "struct",
        fields=_fields(
            (1, "name", "string"),
            (2, "pos", "point"),
            (3, "tags", "list_string", True),
            (4, "attrs", "map_string_u64", True),
            (5, "shape", "shape", True),
        ),
    ),
    "all_scalars": _t(
        "struct",
        fields=_fields(
            (1, "flag", "bool"),
            (2, "small", "u8"),
            (3, "count", "u64"),
            (4, "delta", "i64"),
            (5, "ratio32", "f32"),
            (6, "ratio64", "f64"),
            (7, "label", "string"),
            (8, "blob", "bytes"),
            (9, "ref", "id"),
            (10, "hash", "digest"),
            (11, "tint", "color"),
        ),
    ),
    "recs": _t("list", element="rec_ab"),
    "map_string_rec": _t("map", key="string", value="rec_ab"),
    "scalar": _t(
        "variant",
        cases=_cases(
            (1, "flag", "bool"),
            (2, "count", "u64"),
            (3, "delta", "i64"),
            (4, "ratio", "f64"),
            (5, "label", "string"),
            (6, "blob", "bytes"),
            (7, "ref", "id"),
            (8, "hash", "digest"),
        ),
    ),
    "shape": _t("variant", cases=_cases((1, "circle", "f64"), (2, "rect", "point"), (3, "none", "empty"))),
    "wide_variant": _t(
        "variant",
        cases=_cases((0, "c0", "u64"), (127, "c127", "u64"), (128, "c128", "u64"), (4294967295, "cmax", "u64")),
    ),
    "oset_string": _t("oset", element="string"),
    "oset_u64": _t("oset", element="u64"),
    "oset_rec": _t("oset", element="rec_ab"),
    "deep_list": _t("list", element="deep_list"),
    "deep_struct": _t("struct", fields=_fields((1, "next", "deep_struct", True))),
    "deep_map": _t("map", key="string", value="deep_map"),
    "deep_variant": _t("variant", cases=_cases((1, "next", "deep_variant"), (2, "end", "bool"))),
    "deep_oset": _t("oset", element="deep_oset"),
    "deep_alt_list": _t("list", element="deep_alt_struct"),
    "deep_alt_struct": _t("struct", fields=_fields((1, "next", "deep_alt_list", True))),
}


# ---- value helpers ------------------------------------------------------------------------------------------

def f32_of(x):
    return F32(struct.unpack("<f", struct.pack("<f", x))[0])


def f32_bits(bits):
    return F32(struct.unpack("<f", bits.to_bytes(4, "little"))[0])


def f64_bits(bits):
    return F64(struct.unpack("<d", bits.to_bytes(8, "little"))[0])


def mkid(cls, scheme=1, seed=0):
    return bytes([cls, scheme]) + bytes((seed + i) & 0xFF for i in range(14))


def eid(seed):
    return mkid(12, 1, seed)


def uints(*ns):
    return List(tuple(Uint(n) for n in ns))


def deep(kind, d):
    """A value nested d containers deep of the given deep_* kind (the innermost container is empty/terminal)."""
    if kind == "alt":
        return _deep_alt(d, True)
    inner = {
        "list": List(()),
        "struct": Struct(()),
        "map": Map(()),
        "variant": Variant(2, True),
        "oset": Oset(()),
    }[kind]
    for _ in range(d - 1):
        inner = {
            "list": lambda x: List((x,)),
            "struct": lambda x: Struct(((1, x),)),
            "map": lambda x: Map(((Str("k"), x),)),
            "variant": lambda x: Variant(1, x),
            "oset": lambda x: Oset(((eid(1), b"", x),)),
        }[kind](inner)
    return inner


def _deep_alt(d, is_list):
    if d == 1:
        return List(()) if is_list else Struct(())
    child = _deep_alt(d - 1, not is_list)
    return List((child,)) if is_list else Struct(((1, child),))


# ---- accept vectors -----------------------------------------------------------------------------------------

def _accept_specs():
    """[(name, schema type, value)] in file order; encoded and projected by the reference."""
    specs = []

    def add(name, type_name, value):
        specs.append((name, type_name, value))

    add("bool_false", "bool", False)
    add("bool_true", "bool", True)

    boundaries = {0, 1, 2, 63, 64, 126, 127, 128, 129, 255, 256, 300, 2**32 - 1, 2**32, 2**53 - 1, 2**53, 2**53 + 1}
    for k in range(1, 10):
        boundaries |= {2 ** (7 * k) - 1, 2 ** (7 * k)}
    boundaries |= {2**63 - 1, 2**63, 2**64 - 2, 2**64 - 1}
    for n in sorted(boundaries):
        add("uint_%d" % n, "u64", Uint(n))
    add("uint_u8_0", "u8", Uint(0))
    add("uint_u8_255", "u8", Uint(255))

    nints = [-1, -2, -63, -64, -65, -127, -128, -129, -16384, -16385, -(2**31), -(2**31) - 1, -(2**32)]
    nints += [-(2**53 - 1), -(2**53), -(2**53) - 1, -(2**63) + 1, -(2**63)]
    for n in nints:
        add("nint_m%d" % -n, "i64", Nint(n))
    add("nint_m2p64_wire_min", "i65", Nint(-(2**64)))
    add("int_pos_in_i64", "i64", Uint(5))
    add("int_pos_i64_max", "i64", Uint(2**63 - 1))
    add("int_pos_i65_max", "i65", Uint(2**64 - 1))

    f32_cases = [
        ("zero", F32(0.0)),
        ("neg_zero_input", f32_bits(0x80000000)),
        ("one", F32(1.0)),
        ("neg_one", F32(-1.0)),
        ("one_and_half", F32(1.5)),
        ("tenth", f32_of(0.1)),
        ("max", f32_bits(0x7F7FFFFF)),
        ("neg_max", f32_bits(0xFF7FFFFF)),
        ("min_normal", f32_bits(0x00800000)),
        ("max_subnormal", f32_bits(0x007FFFFF)),
        ("min_subnormal", f32_bits(0x00000001)),
        ("neg_min_subnormal", f32_bits(0x80000001)),
        ("pi", f32_of(3.141592653589793)),
        ("two_p24", F32(16777216.0)),
        ("e15", f32_of(1e15)),
        ("e16", f32_of(1e16)),
        ("e_minus4", f32_of(1e-4)),
        ("e_minus5", f32_of(1e-5)),
        ("hundred", F32(100.0)),
    ]
    for name, value in f32_cases:
        add("f32_" + name, "f32", value)
    f64_cases = [
        ("zero", F64(0.0)),
        ("neg_zero_input", f64_bits(0x8000000000000000)),
        ("one", F64(1.0)),
        ("neg_one", F64(-1.0)),
        ("one_and_half", F64(1.5)),
        ("tenth", F64(0.1)),
        ("point_three_off", F64(0.1 + 0.2)),
        ("max", f64_bits(0x7FEFFFFFFFFFFFFF)),
        ("neg_max", f64_bits(0xFFEFFFFFFFFFFFFF)),
        ("min_normal", f64_bits(0x0010000000000000)),
        ("max_subnormal", f64_bits(0x000FFFFFFFFFFFFF)),
        ("min_subnormal", f64_bits(0x0000000000000001)),
        ("neg_min_subnormal", f64_bits(0x8000000000000001)),
        ("pi", F64(3.141592653589793)),
        ("two_p53", F64(9007199254740992.0)),
        ("two_p53_plus2", F64(9007199254740994.0)),
        ("e15", F64(1e15)),
        ("e16", F64(1e16)),
        ("e22", F64(1e22)),
        ("e_minus4", F64(1e-4)),
        ("e_minus5", F64(1e-5)),
        ("frac", F64(123456789.125)),
        ("hundred", F64(100.0)),
    ]
    for name, value in f64_cases:
        add("f64_" + name, "f64", value)

    strings = [
        ("empty", ""),
        ("a", "a"),
        ("hello", "hello world"),
        ("two_byte", "é"),
        ("three_byte", "€"),
        ("four_byte", "\U0001f600"),
        ("mixed", "aé€\U0001f600z"),
        ("del", "\x7f"),
        ("ctl_01", "\x01"),
        ("ctl_1f", "\x1f"),
        ("quote", '"'),
        ("backslash", "\\"),
        ("escapes", "\n\t\r\b\f"),
        ("slash", "a/b"),
        ("line_separator", " "),
        ("bom", "﻿"),
        ("max_scalar", "\U0010ffff"),
        ("before_surrogates", "퟿"),
        ("after_surrogates", ""),
        ("replacement", "�"),
        ("noncharacter", "￾"),
        ("nfc", "é"),
        ("nfd", "é"),
        ("len_127", "x" * 127),
        ("len_128", "x" * 128),
        ("len_300", "x" * 300),
        ("two_byte_x100", "é" * 100),
    ]
    for name, s in strings:
        add("str_" + name, "string", Str(s))

    add("bytes_empty", "bytes", Bytes(b""))
    add("bytes_zero", "bytes", Bytes(b"\x00"))
    add("bytes_ff", "bytes", Bytes(b"\xff"))
    add("bytes_all_values", "bytes", Bytes(bytes(range(256))))
    add("bytes_len_127", "bytes", Bytes(b"\x00" * 127))
    add("bytes_len_128", "bytes", Bytes(b"\xaa" * 128))

    for cls in range(1, 16):
        add("id_class_%02d" % cls, "id", Id(mkid(cls, (cls % 4) + 1, cls * 3)))
    add("id_max_payload", "id", Id(bytes([15, 4]) + b"\xff" * 14))
    add("id_zero_payload", "id", Id(bytes([1, 3]) + b"\x00" * 14))

    add("digest_sha256_abc", "digest", Digest(1, bytes.fromhex(_sha256_hex(b"abc"))))
    add("digest_zero", "digest", Digest(1, b"\x00" * 32))
    add("digest_ff", "digest", Digest(1, b"\xff" * 32))
    add("digest_blake3_reserved", "digest", Digest(2, bytes(range(32))))

    add("enum_red", "color", Uint(0))
    add("enum_blue", "color", Uint(2))

    add("list_empty", "list_u64", uints())
    add("list_one", "list_u64", uints(7))
    add("list_boundaries", "list_u64", uints(0, 1, 127, 128, 16384))
    add("list_127_items", "list_u64", uints(*range(127)))
    add("list_128_items", "list_u64", uints(*range(128)))
    add("list_nested", "list_list_u64", List((uints(1), uints(2, 3), uints())))
    add("list_strings", "list_string", List((Str(""), Str("a"), Str("é"))))
    add("list_every_scalar_tag", "list_scalar", _every_scalar())
    add("list_neg_zero_inside", "list_scalar", List((Variant(4, f64_bits(0x8000000000000000)),)))

    add("map_empty", "map_string_u64", Map(()))
    add("map_one", "map_string_u64", Map(((Str("k"), Uint(1)),)))
    keys = ["b", "a", "aa", "é", "z", "ab", ""]
    add("map_string_keys_unsorted", "map_string_u64", Map(tuple((Str(k), Uint(i)) for i, k in enumerate(keys))))
    add("map_string_len_before_content", "map_string_u64", Map(((Str("aa"), Uint(1)), (Str("b"), Uint(2)))))
    ukeys = [129, 16384, 0, 127, 128, 255, 1, 2**64 - 1]
    add("map_uint_keys_byte_order", "map_u64_string", Map(tuple((Uint(k), Str(str(k))) for k in ukeys)))
    ikeys = [-1, 0, -(2**63), 5, -129, 127, -2]
    ints = [Uint(k) if k >= 0 else Nint(k) for k in ikeys]
    add("map_int_keys_mixed", "map_i64_u64", Map(tuple((k, Uint(i)) for i, k in enumerate(ints))))
    add("map_id_keys", "map_id_string", Map(tuple((Id(mkid(c, 1, c)), Str("c%d" % c)) for c in (9, 3, 12, 1))))
    add("map_struct_values", "map_string_point", Map(((Str("o"), _point(0.0, 0.0)), (Str("a"), _point(1.5, -2.5)))))
    add("map_nested_unknown", "map_string_rec", Map(((Str("r"), Struct(((1, Uint(1)), (9, Str("x"))))),)))
    names = ["k%03d" % i for i in range(128)]
    names = names[64:] + names[:64]
    add("map_128_entries", "map_string_u64", Map(tuple((Str(k), Uint(i)) for i, k in enumerate(names))))

    add("struct_empty", "empty", Struct(()))
    add("struct_point_unsorted_input", "point", Struct(((2, F64(2.0)), (1, F64(1.0)))))
    add("struct_optional_absent", "rec_ab", Struct(((1, Uint(7)),)))
    add("struct_optional_present", "rec_ab", Struct(((2, Str("two")), (1, Uint(7)))))
    add("struct_default_absent", "cfg", Struct(()))
    add("struct_default_written", "cfg", Struct(((1, Uint(3)), (2, Str("")))))
    wide = ((4294967295, 5), (16384, 4), (128, 3), (127, 2), (0, 1))
    add("struct_wide_field_numbers", "wide", Struct(tuple((no, Uint(v)) for no, v in wide)))
    add("struct_user_minimal", "user", Struct(((1, Str("u")), (2, _point(0.0, 1.0)))))
    user = (
        (5, Variant(2, _point(2.0, 3.0))),
        (4, Map(((Str("hp"), Uint(10)), (Str("ap"), Uint(3))))),
        (3, List((Str("x"), Str("y")))),
        (2, _point(-1.0, 0.5)),
        (1, Str("player")),
    )
    add("struct_user_full", "user", Struct(user))
    add("struct_all_scalars", "all_scalars", _all_scalars())
    add("unknown_fields_scalar", "rec_ab", Struct(((1, Uint(5)), (2, Str("b")), (7, Uint(1)), (9, Str("future")))))
    add("unknown_fields_only", "rec_ab", Struct(((1, Uint(5)), (50, Nint(-1)), (4294967295, True))))
    add("unknown_field_struct", "rec_ab", Struct(((1, Uint(5)), (100, Struct(((1, List((Uint(1),))),))))))
    add("unknown_in_list", "recs", List((Struct(((1, Uint(1)),)), Struct(((1, Uint(2)), (3, Bytes(b"\x01")))))))
    add("unknown_in_oset", "oset_rec", Oset(((eid(1), b"a", Struct(((1, Uint(1)), (6, F64(2.5))))),)))

    add("variant_circle", "shape", Variant(1, F64(2.5)))
    add("variant_rect", "shape", Variant(2, _point(1.0, 2.0)))
    add("variant_none", "shape", Variant(3, Struct(())))
    add("variant_case_0", "wide_variant", Variant(0, Uint(1)))
    add("variant_case_127", "wide_variant", Variant(127, Uint(2)))
    add("variant_case_128", "wide_variant", Variant(128, Uint(3)))
    add("variant_case_u32_max", "wide_variant", Variant(4294967295, Uint(4)))
    add("variant_unknown_case", "shape", Variant(9, Str("future")))
    add("variant_unknown_case_struct", "shape", Variant(40, Struct(((1, Uint(1)),))))

    add("oset_empty", "oset_string", Oset(()))
    add("oset_one", "oset_string", Oset(((eid(1), b"m", Str("one")),)))
    ents = ((eid(3), b"z", Str("c")), (eid(1), b"m", Str("a")), (eid(2), b"a", Str("b")))
    add("oset_distinct_keys_unsorted", "oset_string", Oset(ents))
    ties = tuple((eid(s), b"\x80", Uint(s)) for s in (9, 3, 7, 1, 5))
    add("oset_ties_by_element_id", "oset_u64", Oset(ties))
    add("oset_empty_key", "oset_string", Oset(((eid(2), b"", Str("x")), (eid(1), b"", Str("y")))))
    lex = (b"\x7f\xff", b"\x80\x00", b"\x01", b"\x00\x00", b"\x00", b"")
    add("oset_key_bytewise_order", "oset_u64", Oset(tuple((eid(i + 1), k, Uint(i)) for i, k in enumerate(lex))))
    add("oset_key_prefix_order", "oset_u64", Oset(((eid(1), b"a\x00", Uint(1)), (eid(2), b"a", Uint(2)))))
    recs = ((eid(2), b"b", Struct(((1, Uint(2)),))), (eid(1), b"a", Struct(((1, Uint(1)),))))
    add("oset_struct_values", "oset_rec", Oset(recs))
    mixed_class = (mkid(5, 2, 1), mkid(12, 1, 1))
    add("oset_element_classes", "oset_u64", Oset(tuple((e, b"k", Uint(i)) for i, e in enumerate(mixed_class))))
    add("oset_128_entries", "oset_u64", Oset(tuple((mkid(12, 1, i), bytes([255 - i]), Uint(i)) for i in range(128))))
    add("oset_long_key", "oset_u64", Oset(((eid(1), b"\xab" * 200, Uint(1)),)))

    for d in (1, 2, 32, 63, 64):
        add("depth_list_%d" % d, "deep_list", deep("list", d))
    for kind in ("struct", "map", "variant", "oset", "alt"):
        type_name = "deep_alt_list" if kind == "alt" else "deep_" + kind
        add("depth_%s_63" % kind, type_name, deep(kind, 63))
        add("depth_%s_64" % kind, type_name, deep(kind, 64))
    return specs


def _sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def _point(x, y):
    return Struct(((1, F64(x)), (2, F64(y))))


def _every_scalar():
    return List(
        (
            Variant(1, True),
            Variant(1, False),
            Variant(2, Uint(300)),
            Variant(3, Nint(-300)),
            Variant(4, F64(0.5)),
            Variant(5, Str("s")),
            Variant(6, Bytes(b"\x01\x02")),
            Variant(7, Id(mkid(4, 1, 0))),
            Variant(8, Digest(1, b"\x11" * 32)),
        )
    )


def _all_scalars():
    return Struct(
        (
            (1, True),
            (2, Uint(200)),
            (3, Uint(2**40)),
            (4, Nint(-(2**40))),
            (5, F32(0.25)),
            (6, F64(-0.125)),
            (7, Str("label")),
            (8, Bytes(b"\xde\xad")),
            (9, Id(mkid(4, 2, 7))),
            (10, Digest(1, bytes(range(32)))),
            (11, Uint(1)),
        )
    )


def accept_vectors(schemas=None):
    schemas = SCHEMAS if schemas is None else schemas
    vectors = []
    for name, type_name, value in _accept_specs():
        payload = slb_ref.encode(value)
        vectors.append(
            {
                "name": name,
                "type": type_name,
                "value": slb_ref.value_to_json(value),
                "hex": payload.hex(),
                "oid": slb_ref.oid_text(slb_ref.oid(RECORD_KIND, payload)),
                "text": slb_text.to_text(value, type_name, schemas),
            }
        )
    return vectors


# ---- reject vectors -----------------------------------------------------------------------------------------

def B(*parts):
    """Concatenate hex strings and byte strings into bytes."""
    return b"".join(bytes.fromhex(p) if isinstance(p, str) else p for p in parts)


def _raw_oset(entries):
    """(bytes, [offset of each entry]) of a raw oset body: entries are (element id, order key, value bytes)."""
    out = B("0f") + slb_ref.varint(len(entries))
    offsets = []
    for e, key, value in entries:
        offsets.append(len(out))
        out += e + slb_ref.varint(len(key)) + key + value
    return out, offsets


FALSE = b"\x01"
FRAME_HEAD = B("534c464f")


def _reject_specs():
    """[(name, bytes, rule, offset, target, policy, type)]."""
    out = []

    def add(name, data, rule, offset, target="value", policy="preserve", type_name=None):
        out.append((name, bytes(data), rule, offset, target, policy, type_name))

    # R-SER-TAG
    for tag in ("00", "10", "ff", "80"):
        add("tag_%s_root" % tag, B(tag), "R-SER-TAG", 0)
    add("tag_00_in_list", B("0b0100"), "R-SER-TAG", 2)
    add("tag_10_in_struct_value", B("0d010110"), "R-SER-TAG", 3)
    add("tag_10_in_map_value", B("0c0107016110"), "R-SER-TAG", 5)
    add("tag_00_in_variant", B("0e0100"), "R-SER-TAG", 2)
    body, offs = _raw_oset([(mkid(12), b"", B("10"))])
    add("tag_10_in_oset_value", body, "R-SER-TAG", len(body) - 1)
    bad_keys = (
        ("bool", "01"),
        ("f64", "060000000000000000"),
        ("list", "0b00"),
        ("bytes", "0800"),
        ("digest", "0a01" + "00" * 32),
    )
    for name, key in bad_keys:
        add("map_key_tag_%s" % name, B("0c01", key, "01"), "R-SER-TAG", 2)

    # R-SER-VARINT
    add("varint_uint_overlong_zero", B("038000"), "R-SER-VARINT", 1)
    add("varint_uint_overlong_127", B("03ff00"), "R-SER-VARINT", 1)
    add("varint_uint_overlong_128", B("03808100"), "R-SER-VARINT", 1)
    add("varint_uint_overlong_10_bytes", B("03", "80" * 9, "00"), "R-SER-VARINT", 1)
    add("varint_uint_65_bits_02", B("03", "ff" * 9, "02"), "R-SER-VARINT", 1)
    add("varint_uint_65_bits_7f", B("03", "ff" * 9, "7f"), "R-SER-VARINT", 1)
    add("varint_uint_10th_byte_continues", B("03", "ff" * 9, "81"), "R-SER-VARINT", 1)
    add("varint_uint_11_bytes", B("03", "80" * 10, "01"), "R-SER-VARINT", 1)
    add("varint_nint_overlong", B("048000"), "R-SER-VARINT", 1)
    add("varint_string_length_overlong", B("078000"), "R-SER-VARINT", 1)
    add("varint_bytes_length_overlong", B("088000"), "R-SER-VARINT", 1)
    add("varint_list_count_overlong", B("0b8000"), "R-SER-VARINT", 1)
    add("varint_map_count_overlong", B("0c8000"), "R-SER-VARINT", 1)
    add("varint_struct_count_overlong", B("0d8000"), "R-SER-VARINT", 1)
    add("varint_struct_field_overlong", B("0d018000"), "R-SER-VARINT", 2)
    add("varint_variant_case_overlong", B("0e8000"), "R-SER-VARINT", 1)
    add("varint_oset_count_overlong", B("0f8000"), "R-SER-VARINT", 1)
    body = B("0f01", mkid(12), "8000")
    add("varint_oset_key_length_overlong", body, "R-SER-VARINT", 18)
    add("varint_in_list_item", B("0b01038000"), "R-SER-VARINT", 3)
    add("varint_frame_length_overlong", B(FRAME_HEAD, "01000200", "8000"), "R-SER-VARINT", 8, "frame")

    # R-SER-ORDER
    add("order_map_string_keys", B("0c02", "07016201", "07016101"), "R-SER-ORDER", 6)
    add("order_map_string_length_first", B("0c02", "0702616101", "07016201"), "R-SER-ORDER", 7)
    add("order_map_uint_keys", B("0c02", "030101", "030001"), "R-SER-ORDER", 5)
    add("order_map_uint_byte_order_not_numeric", B("0c02", "038101" + "01", "03808001" + "01"), "R-SER-ORDER", 6)
    add("order_map_nint_before_uint", B("0c02", "040001", "030001"), "R-SER-ORDER", 5)
    add("order_map_id_keys", B("0c02", "09", mkid(2), "01", "09", mkid(1), "01"), "R-SER-ORDER", 20)
    add("order_struct_fields", B("0d02", "0201", "0101"), "R-SER-ORDER", 4)
    add("order_struct_fields_numeric_not_byte_order", B("0d02", "800101", "7f01"), "R-SER-ORDER", 5)
    body, offs = _raw_oset([(mkid(12, 1, 1), b"b", FALSE), (mkid(12, 1, 2), b"a", FALSE)])
    add("order_oset_keys", body, "R-SER-ORDER", offs[1])
    body, offs = _raw_oset([(mkid(12, 1, 2), b"a", FALSE), (mkid(12, 1, 1), b"a", FALSE)])
    add("order_oset_ties_by_element_id", body, "R-SER-ORDER", offs[1])
    body, offs = _raw_oset([(mkid(12, 1, 1), b"\x00", FALSE), (mkid(12, 1, 2), b"", FALSE)])
    add("order_oset_empty_key_last", body, "R-SER-ORDER", offs[1])
    body, offs = _raw_oset([(mkid(12, 1, 1), b"a\x00", FALSE), (mkid(12, 1, 2), b"a", FALSE)])
    add("order_oset_longer_key_first", body, "R-SER-ORDER", offs[1])

    # R-SER-DUP
    add("dup_map_string_key", B("0c02", "07016101", "07016101"), "R-SER-DUP", 6)
    add("dup_map_uint_key", B("0c02", "030701", "030701"), "R-SER-DUP", 5)
    add("dup_map_third_entry", B("0c03", "07016101", "07016201", "07016201"), "R-SER-DUP", 10)
    add("dup_struct_field", B("0d02", "0101", "0101"), "R-SER-DUP", 4)
    add("dup_struct_field_zero", B("0d02", "0001", "0001"), "R-SER-DUP", 4)
    body, offs = _raw_oset([(mkid(12, 1, 1), b"a", FALSE), (mkid(12, 1, 1), b"a", FALSE)])
    add("dup_oset_entry", body, "R-SER-DUP", offs[1])
    body, offs = _raw_oset([(mkid(12, 1, 1), b"a", FALSE), (mkid(12, 1, 1), b"b", FALSE)])
    add("dup_oset_element_other_key", body, "R-SER-DUP", offs[1])
    entries = [(mkid(12, 1, 1), b"a", FALSE), (mkid(12, 1, 2), b"b", FALSE), (mkid(12, 1, 1), b"c", FALSE)]
    body, offs = _raw_oset(entries)
    add("dup_oset_element_nonadjacent", body, "R-SER-DUP", offs[2])

    # R-SER-UTF8 (offset: first content byte)
    cases = [
        ("invalid_ff", "ff"),
        ("lone_continuation", "80"),
        ("truncated_sequence", "c3"),
        ("overlong_c0_80", "c080"),
        ("overlong_c1_bf", "c1bf"),
        ("overlong_e0_80_80", "e08080"),
        ("overlong_f0_80_80_80", "f0808080"),
        ("surrogate_d800", "eda080"),
        ("surrogate_dfff", "edbfbf"),
        ("cesu8_pair", "eda080edb080"),
        ("above_10ffff", "f4908080"),
        ("lead_f5", "f5808080"),
        ("bad_second_byte", "c328"),
        ("cut_euro", "e282"),
        ("nul", "00"),
        ("nul_inside", "610062"),
    ]
    for name, content in cases:
        raw = B(content)
        add("utf8_" + name, B("07", slb_ref.varint(len(raw)), raw), "R-SER-UTF8", 1 + len(slb_ref.varint(len(raw))))
    add("utf8_map_key", B("0c01", "0701ff", "01"), "R-SER-UTF8", 4)
    add("utf8_list_item", B("0b01", "0701ff"), "R-SER-UTF8", 4)
    add("utf8_after_valid_item", B("0b02", "070161", "0701ff"), "R-SER-UTF8", 7)

    # R-SER-NAN
    nans = [
        ("f64_quiet_nan", "06", 0x7FF8000000000000, 8),
        ("f64_signalling_nan", "06", 0x7FF0000000000001, 8),
        ("f64_negative_nan", "06", 0xFFF8000000000000, 8),
        ("f64_max_payload_nan", "06", 0x7FFFFFFFFFFFFFFF, 8),
        ("f64_positive_infinity", "06", 0x7FF0000000000000, 8),
        ("f64_negative_infinity", "06", 0xFFF0000000000000, 8),
        ("f32_quiet_nan", "05", 0x7FC00000, 4),
        ("f32_signalling_nan", "05", 0x7F800001, 4),
        ("f32_negative_nan", "05", 0xFFC00000, 4),
        ("f32_positive_infinity", "05", 0x7F800000, 4),
        ("f32_negative_infinity", "05", 0xFF800000, 4),
    ]
    for name, tag, bits, width in nans:
        add("nan_" + name, B(tag, bits.to_bytes(width, "little")), "R-SER-NAN", 0)
    add("nan_f64_in_list", B("0b01", "06", (0x7FF8000000000000).to_bytes(8, "little")), "R-SER-NAN", 2)
    add("nan_f32_in_struct", B("0d0101", "05", (0x7FC00000).to_bytes(4, "little")), "R-SER-NAN", 3)

    # R-SER-NEGZERO
    add("negzero_f64", B("06", (1 << 63).to_bytes(8, "little")), "R-SER-NEGZERO", 0)
    add("negzero_f32", B("05", (1 << 31).to_bytes(4, "little")), "R-SER-NEGZERO", 0)
    add("negzero_f64_in_list", B("0b01", "06", (1 << 63).to_bytes(8, "little")), "R-SER-NEGZERO", 2)
    add("negzero_f32_map_value", B("0c01", "0300", "05", (1 << 31).to_bytes(4, "little")), "R-SER-NEGZERO", 4)

    # R-SER-TRUNC
    specific = [
        ("string_length_exceeds_input", "07056162"),
        ("bytes_length_exceeds_input", "080301"),
        ("varint_cut_after_continuation", "0380"),
        ("varint_cut_after_two", "03ffff"),
        ("nint_varint_cut", "0480"),
        ("list_count_exceeds_items", "0b030101"),
        ("f64_cut", "0600000000"),
        ("f32_cut", "05000000"),
        ("digest_without_algo", "0a"),
        ("digest_31_bytes", "0a01" + "00" * 31),
        ("id_15_bytes", "09" + "00" * 15),
        ("map_value_missing", "0c01070161"),
        ("struct_value_missing", "0d0101"),
        ("variant_value_missing", "0e01"),
        ("list_count_2p24_exactly", "0b80808008"),
        ("string_length_2p24_exactly", "0780808008"),
        ("bytes_length_2p24_exactly", "0880808008"),
        ("struct_count_2p24_exactly", "0d80808008"),
        ("oset_count_2p24_exactly", "0f80808008"),
    ]
    for name, hx in specific:
        add("trunc_" + name, B(hx), "R-SER-TRUNC", len(B(hx)))
    add("trunc_oset_key_missing", B("0f01", mkid(12), ""), "R-SER-TRUNC", 18)
    for label, sample in _trunc_samples():
        full = slb_ref.encode(sample)
        for n in range(0 if label == "user" else 1, len(full)):
            add("trunc_%s_%02d" % (label, n), full[:n], "R-SER-TRUNC", n)
    add("trunc_frame_magic_partial", B("534c"), "R-SER-TRUNC", 2, "frame")
    add("trunc_frame_header_partial", B("534c464f010002"), "R-SER-TRUNC", 7, "frame")
    add("trunc_frame_no_length", B(FRAME_HEAD, "01000200"), "R-SER-TRUNC", 8, "frame")
    add("trunc_frame_payload_short", B(FRAME_HEAD, "01000200", "05", "aabb"), "R-SER-TRUNC", 11, "frame")
    add("trunc_frame_empty", b"", "R-SER-TRUNC", 0, "frame")

    # R-SER-TRAILING
    add("trailing_after_false", B("0100"), "R-SER-TRAILING", 1)
    add("trailing_after_uint", B("0305ff"), "R-SER-TRAILING", 2)
    add("trailing_two_values", B("0101"), "R-SER-TRAILING", 1)
    add("trailing_after_empty_list", B("0b0000"), "R-SER-TRAILING", 2)
    add("trailing_after_string", B("07016100"), "R-SER-TRAILING", 3)
    add("trailing_after_f64", B("06", "00" * 8, "00"), "R-SER-TRAILING", 9)
    add("trailing_after_struct", B("0d010101", "01"), "R-SER-TRAILING", 4)
    add("trailing_after_map", B("0c00", "00"), "R-SER-TRAILING", 2)
    add("trailing_frame_byte", B(FRAME_HEAD, "01000200", "00", "00"), "R-SER-TRAILING", 9, "frame")

    # R-SER-LIMIT
    add("limit_depth_65_lists", B("0b01" * 64, "0b00"), "R-SER-LIMIT", 128)
    add("limit_depth_65_lists_scalar_inside", B("0b01" * 65, "01"), "R-SER-LIMIT", 128)
    add("limit_depth_65_structs", B("0d0101" * 64, "0d00"), "R-SER-LIMIT", 192)
    add("limit_depth_65_maps", B("0c0107016b" * 64, "0c00"), "R-SER-LIMIT", 320)
    add("limit_depth_65_variants", B("0e01" * 64, "0e0101"), "R-SER-LIMIT", 128)
    level = B("0f01", mkid(12, 1, 1), "00")
    add("limit_depth_65_osets", B(level * 64, "0f00"), "R-SER-LIMIT", 19 * 64)
    add("limit_depth_65_alternating", B(B("0b01", "0d0101") * 32, "0b00"), "R-SER-LIMIT", 160)
    over = "81808008"
    for name, tag in (("list", "0b"), ("map", "0c"), ("struct", "0d"), ("oset", "0f")):
        add("limit_%s_count_2p24_plus1" % name, B(tag, over), "R-SER-LIMIT", 1)
    add("limit_string_length_2p24_plus1", B("07", over), "R-SER-LIMIT", 1)
    add("limit_bytes_length_2p24_plus1", B("08", over), "R-SER-LIMIT", 1)
    add("limit_list_count_u64_max", B("0b", "ff" * 9, "01"), "R-SER-LIMIT", 1)
    add("limit_string_length_u64_max", B("07", "ff" * 9, "01"), "R-SER-LIMIT", 1)
    add("limit_oset_key_length_2p24_plus1", B("0f01", mkid(12), over), "R-SER-LIMIT", 18)
    add("limit_struct_field_number_2p32", B("0d01", "8080808010", "01"), "R-SER-LIMIT", 2)
    add("limit_variant_case_2p32", B("0e", "8080808010", "01"), "R-SER-LIMIT", 1)
    add("limit_frame_payload_length_2p24_plus1", B(FRAME_HEAD, "01000200", over), "R-SER-LIMIT", 8, "frame")

    # R-SER-FRAME
    ok = "0100" + "0200" + "00"
    add("frame_bad_magic_last_byte", B("534c4650", ok), "R-SER-FRAME", 0, "frame")
    add("frame_bad_magic_first_byte", B("544c464f", ok), "R-SER-FRAME", 0, "frame")
    add("frame_bad_magic_lowercase", B("736c666f", ok), "R-SER-FRAME", 0, "frame")
    add("frame_bad_magic_partial", B("5358"), "R-SER-FRAME", 0, "frame")
    for major in ("00", "02", "ff"):
        add("frame_major_%s" % major, B(FRAME_HEAD, major, "00", "0200", "00"), "R-SER-FRAME", 4, "frame")
    for kind in ("00", "0b", "ff"):
        add("frame_kind_%s" % kind, B(FRAME_HEAD, "0100", kind, "00", "00"), "R-SER-FRAME", 6, "frame")
    for flags in ("01", "02", "80", "ff"):
        add("frame_flags_%s" % flags, B(FRAME_HEAD, "010002", flags, "00"), "R-SER-FRAME", 7, "frame")

    # R-SER-UNKNOWN (policy strict, schema directed)
    def strict(name, value, type_name, offset):
        add(name, slb_ref.encode(value), "R-SER-UNKNOWN", offset, "value", "strict", type_name)

    strict("unknown_strict_scalar_field", Struct(((1, Uint(5)), (7, Uint(1)))), "rec_ab", 5)
    strict("unknown_strict_after_known", Struct(((1, Uint(5)), (2, Str("b")), (9, Str("future")))), "rec_ab", 9)
    strict("unknown_strict_field_zero", Struct(((0, Uint(5)), (1, Uint(1)))), "rec_ab", 2)
    second = Struct(((1, Uint(2)), (3, Bytes(b"\x01"))))
    strict("unknown_strict_in_list", List((Struct(((1, Uint(1)),)), second)), "recs", 12)
    nested = Struct(((1, Uint(1)), (9, Str("x"))))
    strict("unknown_strict_in_map_value", Map(((Str("r"), nested),)), "map_string_rec", 10)
    strict("unknown_strict_in_variant", Variant(2, Struct(((1, F64(1.0)), (2, F64(2.0)), (3, F64(3.0))))), "shape", 24)
    strict("unknown_strict_in_oset", Oset(((eid(1), b"a", Struct(((1, Uint(1)), (6, F64(2.5))))),)), "oset_rec", 25)
    return out


def _trunc_samples():
    user = Struct(((1, F64(1.5)), (2, Str("é")), (3, Nint(-129)), (4, Id(mkid(4, 1, 0)))))
    table = Map(((Uint(1), List((Uint(2), Nint(-3)))), (Uint(200), Variant(5, Bytes(b"\x00\x01")))))
    tree = Oset(((eid(1), b"ab", Struct(((1, Digest(1, b"\x22" * 32)),))),))
    return (("user", user), ("table", table), ("tree", tree))


def reject_vectors():
    vectors = []
    for name, data, rule, offset, target, policy, type_name in _reject_specs():
        vectors.append(
            {
                "name": name,
                "hex": data.hex(),
                "rule": rule,
                "offset": offset,
                "target": target,
                "policy": policy,
                "type": type_name,
            }
        )
    return vectors


# ---- files --------------------------------------------------------------------------------------------------

def _line(obj):
    return json.dumps(obj, ensure_ascii=False)


def render_schemas(schemas):
    rows = ",\n".join("%s: %s" % (_line(name), _line(schemas[name])) for name in schemas)
    return '{\n"format": "slb-corpus-schemas/1",\n"types": {\n%s\n}\n}\n' % rows


def render_vectors(fmt, vectors):
    rows = ",\n".join(_line(v) for v in vectors)
    return '{\n"format": "%s",\n"vectors": [\n%s\n]\n}\n' % (fmt, rows)


def render_all():
    return {
        "schemas.json": render_schemas(SCHEMAS),
        "accept.json": render_vectors("slb-accept/1", accept_vectors()),
        "reject.json": render_vectors("slb-reject/1", reject_vectors()),
    }


def write(directory):
    os.makedirs(directory, exist_ok=True)
    for name, text in render_all().items():
        with open(os.path.join(directory, name), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    return []


def _load(directory, name, findings):
    path = os.path.join(directory, name)
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        findings.append("R-GOLD-NOFILE %s: missing or unreadable; run `python3 -I tools/fabric/corpus.py write`" % name)
        return None, None
    try:
        return text, json.loads(text)
    except ValueError as err:
        findings.append("R-GOLD-SHAPE %s: not valid JSON (%s)" % (name, err))
        return None, None


def _bad(findings, rule, fname, vector, message):
    findings.append("%s %s: vector '%s': %s" % (rule, fname, vector.get("name", "?"), message))


def _verify_accept(vector, schemas, findings):
    fname = "accept.json"
    try:
        value = slb_ref.value_from_json(vector["value"])
        payload = slb_ref.encode(value)
        want = vector["hex"]
        if payload.hex() != want:
            _bad(findings, "R-GOLD-HEX", fname, vector, "encode(value) is %s, file has %s" % (payload.hex(), want))
            return
        if slb_ref.oid_text(slb_ref.oid(RECORD_KIND, payload)) != vector["oid"]:
            _bad(findings, "R-GOLD-OID", fname, vector, "oid does not match the record frame around hex")
        again = slb_ref.encode(slb_ref.decode(payload))
        if again != payload:
            _bad(findings, "R-GOLD-DECODE", fname, vector, "decode(hex) re-encodes to different bytes")
        text = slb_text.to_text(value, vector["type"], schemas)
        if text != vector["text"]:
            _bad(findings, "R-GOLD-TEXT", fname, vector, "to_text is %s, file has %s" % (text, vector["text"]))
        back = slb_text.from_text(vector["text"], vector["type"], schemas)
        if slb_ref.encode(back) != payload:
            _bad(findings, "R-GOLD-TEXT", fname, vector, "from_text(text) encodes to different bytes")
        if not vector["name"].startswith("unknown_") and "unknown" not in vector["name"]:
            slb_ref.decode(payload, "strict", schemas, vector["type"])
    except (SlbError, KeyError, ValueError, TypeError) as err:
        _bad(findings, "R-GOLD-DECODE", fname, vector, "%s: %s" % (type(err).__name__, err))


def _verify_reject(vector, schemas, findings):
    fname = "reject.json"
    try:
        data = bytes.fromhex(vector["hex"])
        outcome = "accepted"
        try:
            if vector["target"] == "frame":
                slb_ref.parse_frame(data)
            else:
                schema = schemas if vector["policy"] == "strict" else None
                slb_ref.decode(data, vector["policy"], schema, vector["type"])
        except SlbError as err:
            outcome = (err.rule, err.offset)
        if outcome != (vector["rule"], vector["offset"]):
            want = (vector["rule"], vector["offset"])
            _bad(findings, "R-GOLD-REJECT", fname, vector, "reference gives %s, file expects %s" % (outcome, want))
    except (KeyError, ValueError, TypeError) as err:
        _bad(findings, "R-GOLD-REJECT", fname, vector, "%s: %s" % (type(err).__name__, err))


def _coverage(accept, reject, findings):
    if len(accept) < MIN_ACCEPT:
        findings.append("R-GOLD-COVERAGE accept.json: %d vectors, at least %d required" % (len(accept), MIN_ACCEPT))
    if len(reject) < MIN_REJECT:
        findings.append("R-GOLD-COVERAGE reject.json: %d vectors, at least %d required" % (len(reject), MIN_REJECT))
    for label, vectors in (("accept.json", accept), ("reject.json", reject)):
        names = [v.get("name") for v in vectors]
        for name in sorted({n for n in names if names.count(n) > 1}):
            findings.append("R-GOLD-COVERAGE %s: vector '%s': duplicate name" % (label, name))
    tags = set()
    for vector in accept:
        try:
            _collect_tags(slb_ref.encode(slb_ref.value_from_json(vector["value"])), tags)
        except (SlbError, KeyError, ValueError, TypeError):
            pass
    for tag in range(1, 16):
        if tag not in tags:
            findings.append("R-GOLD-COVERAGE accept.json: no accept vector contains tag %#04x" % tag)
    rules = {v.get("rule") for v in reject}
    for rule in slb_ref.RULES:
        if rule not in rules:
            findings.append("R-GOLD-COVERAGE reject.json: no must-reject vector for %s" % rule)


def _collect_tags(payload, tags):
    value = slb_ref.decode(payload)
    _walk(value, tags)


def _walk(v, tags):
    if v is True:
        tags.add(2)
    elif v is False:
        tags.add(1)
    elif isinstance(v, Uint):
        tags.add(3)
    elif isinstance(v, Nint):
        tags.add(4)
    elif isinstance(v, F32):
        tags.add(5)
    elif isinstance(v, F64):
        tags.add(6)
    elif isinstance(v, Str):
        tags.add(7)
    elif isinstance(v, Bytes):
        tags.add(8)
    elif isinstance(v, Id):
        tags.add(9)
    elif isinstance(v, Digest):
        tags.add(10)
    elif isinstance(v, List):
        tags.add(11)
        for x in v.items:
            _walk(x, tags)
    elif isinstance(v, Map):
        tags.add(12)
        for k, x in v.entries:
            _walk(k, tags)
            _walk(x, tags)
    elif isinstance(v, Struct):
        tags.add(13)
        for _, x in v.fields:
            _walk(x, tags)
    elif isinstance(v, Variant):
        tags.add(14)
        _walk(v.value, tags)
    elif isinstance(v, Oset):
        tags.add(15)
        for _, _, x in v.entries:
            _walk(x, tags)


def _first_difference(want, got):
    """Name the first differing vector between two lists of vector dicts, or None."""
    by_name = {v.get("name"): v for v in got}
    for vector in want:
        other = by_name.get(vector["name"])
        if other is None:
            return vector["name"], "missing from the file"
        for key in vector:
            if vector[key] != other.get(key):
                note = "field '%s' is %s, regenerated %s" % (key, _short(other.get(key)), _short(vector[key]))
                return vector["name"], note
    for vector in got:
        if vector.get("name") not in {w["name"] for w in want}:
            return vector.get("name"), "not produced by the generator"
    return None


def _short(x):
    text = json.dumps(x, ensure_ascii=False)
    return text if len(text) <= 80 else text[:77] + "..."


def check(directory):
    findings = []
    texts = {}
    docs = {}
    for name in FILES:
        texts[name], docs[name] = _load(directory, name, findings)
    if findings:
        return findings
    schemas_doc = docs["schemas.json"]
    schemas = schemas_doc.get("types") if isinstance(schemas_doc, dict) else None
    if not isinstance(schemas, dict):
        return ["R-GOLD-SHAPE schemas.json: expected an object with a types object"]
    for problem in slb_text.check_schemas(schemas):
        findings.append("R-GOLD-SCHEMA schemas.json: %s" % problem)
    for fname, fmt in (("accept.json", "slb-accept/1"), ("reject.json", "slb-reject/1")):
        doc = docs[fname]
        if not isinstance(doc, dict) or doc.get("format") != fmt or not isinstance(doc.get("vectors"), list):
            findings.append('R-GOLD-SHAPE %s: expected {"format": "%s", "vectors": [...]}' % (fname, fmt))
    if findings:
        return findings
    accept, reject = docs["accept.json"]["vectors"], docs["reject.json"]["vectors"]
    for vector in accept:
        _verify_accept(vector, schemas, findings)
    for vector in reject:
        _verify_reject(vector, schemas, findings)
    _coverage(accept, reject, findings)
    regenerated = render_all()
    want_vectors = {"accept.json": accept_vectors(), "reject.json": reject_vectors()}
    for fname in FILES:
        if texts[fname] == regenerated[fname]:
            continue
        if fname == "schemas.json":
            findings.append("R-GOLD-DRIFT schemas.json: differs from the generator (corpus.py SCHEMAS)")
            continue
        diff = _first_difference(want_vectors[fname], docs[fname]["vectors"])
        if diff:
            findings.append("R-GOLD-DRIFT %s: vector '%s': %s" % (fname, diff[0], diff[1]))
        else:
            findings.append("R-GOLD-DRIFT %s: same vectors but not the canonical rendering" % fname)
    if not findings:
        print("fabric-golden-check: ok (%d accept, %d must-reject vectors)" % (len(accept), len(reject)))
    return findings


def main(argv):
    args = list(argv)
    directory = DEFAULT_DIR
    if "--dir" in args:
        i = args.index("--dir")
        if i + 1 >= len(args):
            print("usage: corpus.py check|write [--dir DIR]", file=sys.stderr)
            return 2
        directory = os.path.abspath(args[i + 1])
        del args[i : i + 2]
    if len(args) != 1 or args[0] not in ("check", "write"):
        print("usage: corpus.py check|write [--dir DIR]", file=sys.stderr)
        return 2
    findings = check(directory) if args[0] == "check" else write(directory)
    for line in findings:
        print(line, file=sys.stderr)
    if findings:
        print("fabric-golden-%s: FAIL (%d finding(s))" % (args[0], len(findings)), file=sys.stderr)
        return 1
    if args[0] == "write":
        print("fabric-golden-write: wrote %s" % ", ".join(FILES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
