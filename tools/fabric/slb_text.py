"""Canonical text projection of SLB/1 (WO-038, ARCH1-R29, ADR-0012). Stdlib only (ADR-0004).

A schema-directed JSON projection that is bijective with the binary form: to_text(value, type, schemas) and
from_text(text, type, schemas). Text is an encoding, never a second source of truth.

Corpus schemas (tests/fixtures/fabric/slb/schemas.json): {"format": "slb-corpus-schemas/1", "types": {name: desc}}
where a descriptor is {"kind": ...} and a type reference is a name from `types` or an inline descriptor:
  bool | uint{bits} | int{bits} | f32 | f64 | string | bytes | id | digest | enum{enumerators:[{no,name}]}
  list{element} | map{key: string|uint|int|id, value} | struct{fields:[{no,name,type,optional}]}
  variant{cases:[{no,name,type}]} | oset{element}
`bits` defaults to 64; uint covers 0..2^bits-1 and int covers -2^(bits-1)..2^(bits-1)-1 (bits 65 gives the full
SLB/1 integer range -2^64..2^64-1).

Text rules (the document is compact JSON: no whitespace, strings with ensure_ascii off, only \\" \\\\ \\b \\f \\n \\r
\\t and \\u00xx (lowercase hex) escapes):
  bool        true | false
  uint, int   a JSON integer when |v| <= 2^53-1, else a decimal string ("9007199254740992", "-9007199254740992")
  enum        the enumerator name as a string (an unknown number is a R-TXT-TYPE error)
  f32, f64    a JSON number made of the shortest decimal digits that round-trip to the same width. With n the
              decimal exponent of the leading digit: -4 <= n < 16 is positional ("1.5", "100.0", "0.0001", "0.0"),
              else "d.ddde+XX" / "de-XX" (sign always, exponent at least two digits: "1e+16", "5e-324")
  string      a JSON string
  bytes       lowercase hex string
  id          "<prefix>_<26 Crockford base32 characters, upper case>" (class 1..15, first character 0-7)
  digest      "sha256:<64 hex>" (algo 1) or "blake3:<64 hex>" (algo 2); other algos have no text form
  list        JSON array
  map         JSON array of [key, value] pairs in canonical (encoded key bytes) order
  struct      JSON object, fields by name in field-number order; an unknown field is the key "?<no>" with the
              string "slb:<hex>" of its encoded value; an absent optional field is omitted
  variant     {"<case name>": value} (an unknown case is {"?<case>": "slb:<hex>"})
  oset        JSON array of {"id": element id text, "order": hex, "value": value} in canonical order
from_text accepts exactly the text to_text produces (any other spelling is R-TXT-SYNTAX), so the two directions
are inverse bijections.
"""

import json
import os
import re
import struct
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
from slb_ref import SlbError  # noqa: E402

MAX_SAFE = (1 << 53) - 1
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
ID_CLASSES = {
    1: "prj",
    2: "sch",
    3: "typ",
    4: "ast",
    5: "ent",
    6: "ins",
    7: "drv",
    8: "txn",
    9: "evt",
    10: "act",
    11: "pkg",
    12: "elm",
    13: "nsp",
    14: "ope",
    15: "imp",
}
PREFIX_CLASS = {prefix: cls for cls, prefix in ID_CLASSES.items()}
DIGEST_NAMES = {1: "sha256", 2: "blake3"}
NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
HEX_RE = re.compile(r"^(?:[0-9a-f]{2})*$")


def _err(rule, message):
    return SlbError(rule, None, message)


# ---- ids ----------------------------------------------------------------------------------------------------

def id_to_text(raw):
    if len(raw) != 16 or raw[0] not in ID_CLASSES:
        raise _err("R-TXT-TYPE", "id needs 16 bytes and a class in 1..15")
    n = int.from_bytes(raw, "big")
    chars = []
    for _ in range(26):
        chars.append(CROCKFORD[n & 31])
        n >>= 5
    return ID_CLASSES[raw[0]] + "_" + "".join(reversed(chars))


def id_from_text(text):
    prefix, sep, body = text.partition("_")
    if sep != "_" or prefix not in PREFIX_CLASS or len(body) != 26:
        raise _err("R-TXT-SYNTAX", "bad id text %r" % (text,))
    if any(c not in CROCKFORD for c in body) or body[0] > "7":
        raise _err("R-TXT-SYNTAX", "bad id alphabet or first character in %r" % (text,))
    n = 0
    for c in body:
        n = (n << 5) | CROCKFORD.index(c)
    raw = n.to_bytes(16, "big")
    if raw[0] != PREFIX_CLASS[prefix]:
        raise _err("R-TXT-SYNTAX", "prefix %s does not match class byte %d" % (prefix, raw[0]))
    return raw


# ---- floats -------------------------------------------------------------------------------------------------

def _f32_bits(x):
    return int.from_bytes(struct.pack("<f", x), "little")


def _f32_value(bits):
    return struct.unpack("<f", bits.to_bytes(4, "little"))[0]


def _f32_round_trips(decimal, x):
    """Does the decimal string round to the float32 x (nearest, ties to even)? Exact rational arithmetic."""
    bits = _f32_bits(x)
    exact = Fraction(decimal)
    lo = Fraction(_f32_value(bits - 1))
    hi = Fraction(2) ** 128 if bits == 0x7F7FFFFF else Fraction(_f32_value(bits + 1))
    below = (Fraction(x) + lo) / 2
    above = (Fraction(x) + hi) / 2
    even = bits % 2 == 0
    low_ok = exact > below or (even and exact == below)
    high_ok = exact < above or (even and exact == above)
    return low_ok and high_ok


def _shortest_digits(x, width):
    """(digits, n): the shortest decimal digits (no trailing zero) that round-trip, x = d.ddd * 10^n, x > 0."""
    for places in range(0, 18):
        s = "%.*e" % (places, x)
        ok = float(s) == x if width == 8 else _f32_round_trips(s, x)
        if ok:
            mant, _, exp = s.partition("e")
            digits = mant.replace(".", "").rstrip("0") or "0"
            return digits, int(exp)
    raise _err("R-TXT-TYPE", "no round-trip digits for %r" % (x,))  # pragma: no cover


def float_to_text(x, width):
    if x != x or x in (float("inf"), float("-inf")):
        raise _err("R-TXT-TYPE", "non-finite float")
    if x == 0.0:
        return "0.0"
    sign = "-" if x < 0 else ""
    digits, n = _shortest_digits(abs(x), width)
    k = len(digits)
    if -4 <= n < 16:
        if n >= 0:
            whole = digits[: n + 1].ljust(n + 1, "0")
            frac = digits[n + 1 :] or "0"
            body = whole + "." + frac
        else:
            body = "0." + "0" * (-n - 1) + digits
    else:
        body = digits[0] + ("." + digits[1:] if k > 1 else "") + "e" + ("+" if n >= 0 else "-") + "%02d" % abs(n)
    return sign + body


def float_from_token(token, width):
    if width == 8:
        return float(token)
    negative = token.startswith("-")
    magnitude = token[1:] if negative else token
    d = float(magnitude)
    try:
        bits = _f32_bits(d)
    except OverflowError:
        raise _err("R-TXT-TYPE", "out of f32 range: %s" % token) from None
    exact = Fraction(magnitude)
    best = None
    for cand in (bits - 1, bits, bits + 1):
        if cand < 0 or cand > 0x7F7FFFFF:
            continue
        dist = abs(Fraction(_f32_value(cand)) - exact)
        if best is None or dist < best[0] or (dist == best[0] and cand % 2 == 0):
            best = (dist, cand)
    value = _f32_value(best[1])
    return -value if negative else value


# ---- schema -------------------------------------------------------------------------------------------------

def load_schemas(path):
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("format") != "slb-corpus-schemas/1" or not isinstance(data.get("types"), dict):
        raise _err("R-TXT-TYPE", "%s: expected format slb-corpus-schemas/1 with a types object" % path)
    return data["types"]


def _td(schemas, ref):
    if isinstance(ref, str):
        if ref not in schemas:
            raise _err("R-TXT-TYPE", "unknown schema type %r" % ref)
        return schemas[ref]
    return ref


def check_schemas(schemas):
    """Well-formedness: unique field numbers and names, unique enumerators and cases, known references."""
    problems = []
    for name, desc in sorted(schemas.items()):
        kind = desc.get("kind")
        if kind in ("struct", "variant"):
            entries = desc.get("fields" if kind == "struct" else "cases", [])
            nos = [e["no"] for e in entries]
            names = [e["name"] for e in entries]
            if len(set(nos)) != len(nos) or len(set(names)) != len(names):
                problems.append("%s: duplicate number or name" % name)
            if not all(NAME_RE.match(n) for n in names):
                problems.append("%s: bad name" % name)
            refs = [e["type"] for e in entries]
        elif kind in ("list", "oset"):
            refs = [desc["element"]]
        elif kind == "map":
            refs = [desc["value"]]
            if desc.get("key") not in ("string", "uint", "int", "id"):
                problems.append("%s: bad map key kind" % name)
        elif kind == "enum":
            refs = []
            nos = [e["no"] for e in desc["enumerators"]]
            if len(set(nos)) != len(nos):
                problems.append("%s: duplicate enumerator" % name)
        elif kind in ("bool", "uint", "int", "f32", "f64", "string", "bytes", "id", "digest"):
            refs = []
        else:
            problems.append("%s: unknown kind %r" % (name, kind))
            refs = []
        problems += ["%s: unknown type %r" % (name, r) for r in refs if isinstance(r, str) and r not in schemas]
    return problems


# ---- to_text ------------------------------------------------------------------------------------------------

def _jstr(s):
    return json.dumps(s, ensure_ascii=False)


def _int_text(n):
    return str(n) if abs(n) <= MAX_SAFE else '"%d"' % n


def _bits(td):
    return td.get("bits", 64)


def _mismatch(td, value):
    return _err("R-TXT-TYPE", "value %s does not fit schema kind %s" % (type(value).__name__, td.get("kind")))


def to_text(value, type_ref, schemas):
    td = _td(schemas, type_ref)
    kind = td["kind"]
    if kind == "bool":
        if value is True or value is False:
            return "true" if value else "false"
        raise _mismatch(td, value)
    if kind in ("uint", "int"):
        return _int_text(_int_value(td, value))
    if kind == "enum":
        if isinstance(value, slb_ref.Uint):
            for e in td["enumerators"]:
                if e["no"] == value.value:
                    return _jstr(e["name"])
        raise _mismatch(td, value)
    if kind in ("f32", "f64"):
        cls = slb_ref.F32 if kind == "f32" else slb_ref.F64
        if not isinstance(value, cls):
            raise _mismatch(td, value)
        return float_to_text(value.value, 4 if kind == "f32" else 8)
    if kind == "string":
        if isinstance(value, slb_ref.Str):
            return _jstr(value.value)
        raise _mismatch(td, value)
    if kind == "bytes":
        if isinstance(value, slb_ref.Bytes):
            return '"' + value.value.hex() + '"'
        raise _mismatch(td, value)
    if kind == "id":
        if isinstance(value, slb_ref.Id):
            return '"' + id_to_text(value.value) + '"'
        raise _mismatch(td, value)
    if kind == "digest":
        if isinstance(value, slb_ref.Digest) and value.algo in DIGEST_NAMES:
            return '"%s:%s"' % (DIGEST_NAMES[value.algo], value.value.hex())
        raise _mismatch(td, value)
    return _container_text(td, kind, value, schemas)


def _int_value(td, value):
    bits = _bits(td)
    if td["kind"] == "uint":
        if isinstance(value, slb_ref.Uint) and value.value < (1 << bits):
            return value.value
    else:
        half = 1 << (bits - 1)
        if isinstance(value, (slb_ref.Uint, slb_ref.Nint)) and -half <= value.value < half:
            return value.value
    raise _mismatch(td, value)


def _unknown_text(raw_value):
    return '"slb:%s"' % slb_ref.encode(raw_value).hex()


def _container_text(td, kind, value, schemas):
    if kind == "list":
        if not isinstance(value, slb_ref.List):
            raise _mismatch(td, value)
        return "[" + ",".join(to_text(x, td["element"], schemas) for x in value.items) + "]"
    if kind == "map":
        if not isinstance(value, slb_ref.Map):
            raise _mismatch(td, value)
        key_td = {"kind": td["key"]}
        entries = sorted(((slb_ref.encode(k), k, v) for k, v in value.entries), key=lambda e: e[0])
        pairs = ["[%s,%s]" % (to_text(k, key_td, schemas), to_text(v, td["value"], schemas)) for _, k, v in entries]
        return "[" + ",".join(pairs) + "]"
    if kind == "struct":
        if not isinstance(value, slb_ref.Struct):
            raise _mismatch(td, value)
        by_no = {f["no"]: f for f in td["fields"]}
        parts = []
        present = set()
        for no, v in sorted(value.fields, key=lambda f: f[0]):
            present.add(no)
            if no in by_no:
                parts.append("%s:%s" % (_jstr(by_no[no]["name"]), to_text(v, by_no[no]["type"], schemas)))
            else:
                parts.append("%s:%s" % (_jstr("?%d" % no), _unknown_text(v)))
        for f in td["fields"]:
            if f["no"] not in present and not f.get("optional"):
                raise _err("R-TXT-TYPE", "required field %s is absent" % f["name"])
        return "{" + ",".join(parts) + "}"
    if kind == "variant":
        if not isinstance(value, slb_ref.Variant):
            raise _mismatch(td, value)
        for c in td["cases"]:
            if c["no"] == value.case:
                return "{%s:%s}" % (_jstr(c["name"]), to_text(value.value, c["type"], schemas))
        return "{%s:%s}" % (_jstr("?%d" % value.case), _unknown_text(value.value))
    if kind == "oset":
        if not isinstance(value, slb_ref.Oset):
            raise _mismatch(td, value)
        rows = []
        for eid, okey, v in sorted(value.entries, key=lambda e: (e[1], e[0])):
            inner = to_text(v, td["element"], schemas)
            rows.append('{"id":"%s","order":"%s","value":%s}' % (id_to_text(eid), okey.hex(), inner))
        return "[" + ",".join(rows) + "]"
    raise _err("R-TXT-TYPE", "unknown kind %r" % kind)


# ---- from_text ----------------------------------------------------------------------------------------------

class _Num(str):
    """A JSON number token, kept as written so the canonical form can be compared."""


class _Obj(list):
    """A JSON object as its (key, value) pairs in document order."""


def _object_pairs(pairs):
    keys = [k for k, _ in pairs]
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate object key")
    return _Obj(pairs)


def _bad_constant(name):
    raise ValueError("non-finite number %s" % name)


def from_text(text, type_ref, schemas):
    """Parse canonical text to a value; any text other than the canonical spelling of a value is R-TXT-SYNTAX."""
    try:
        tree = json.loads(
            text,
            parse_int=_Num,
            parse_float=_Num,
            parse_constant=_bad_constant,
            object_pairs_hook=_object_pairs,
        )
    except ValueError as err:
        raise _err("R-TXT-SYNTAX", "not JSON: %s" % err) from None
    value = _build(tree, type_ref, schemas)
    if to_text(value, type_ref, schemas) != text:
        raise _err("R-TXT-SYNTAX", "valid JSON but not the canonical spelling")
    return value


def _syntax(message):
    return _err("R-TXT-SYNTAX", message)


def _int_from(td, node):
    if isinstance(node, _Num):
        if not re.match(r"^-?(0|[1-9][0-9]*)$", node):
            raise _syntax("integer expected: %s" % node)
        n = int(node)
    elif isinstance(node, str) and re.match(r"^-?(0|[1-9][0-9]*)$", node):
        n = int(node)
    else:
        raise _syntax("integer expected")
    if td["kind"] == "uint":
        if not 0 <= n < (1 << _bits(td)):
            raise _err("R-TXT-TYPE", "uint out of range")
        return slb_ref.Uint(n)
    half = 1 << (_bits(td) - 1)
    if not -half <= n < half:
        raise _err("R-TXT-TYPE", "int out of range")
    return slb_ref.Uint(n) if n >= 0 else slb_ref.Nint(n)


def _hex_from(node, size=None):
    if not isinstance(node, str) or isinstance(node, _Num) or not HEX_RE.match(node):
        raise _syntax("lowercase hex string expected")
    raw = bytes.fromhex(node)
    if size is not None and len(raw) != size:
        raise _syntax("expected %d bytes" % size)
    return raw


def _unknown_from(node):
    if not isinstance(node, str) or isinstance(node, _Num) or not node.startswith("slb:"):
        raise _syntax('unknown field value must be a string "slb:<hex>"')
    return slb_ref.decode(_hex_from(node[4:]))


def _build(node, type_ref, schemas):
    td = _td(schemas, type_ref)
    kind = td["kind"]
    if kind == "bool":
        if node is True or node is False:
            return node
        raise _syntax("bool expected")
    if kind in ("uint", "int"):
        return _int_from(td, node)
    if kind in ("f32", "f64"):
        if not isinstance(node, _Num):
            raise _syntax("number expected")
        x = float_from_token(node, 4 if kind == "f32" else 8)
        if x != x or x in (float("inf"), float("-inf")):
            raise _err("R-TXT-TYPE", "non-finite float")
        return slb_ref.F32(x) if kind == "f32" else slb_ref.F64(x)
    if kind in ("string", "enum", "bytes", "id", "digest"):
        if not isinstance(node, str) or isinstance(node, _Num):
            raise _syntax("string expected")
        return _scalar_from_string(td, kind, node)
    return _container_from(td, kind, node, schemas)


def _scalar_from_string(td, kind, node):
    if kind == "string":
        return slb_ref.Str(node)
    if kind == "bytes":
        return slb_ref.Bytes(_hex_from(node))
    if kind == "id":
        return slb_ref.Id(id_from_text(node))
    if kind == "digest":
        name, sep, digest = node.partition(":")
        algos = {v: k for k, v in DIGEST_NAMES.items()}
        if sep != ":" or name not in algos:
            raise _syntax("digest text is sha256:<hex> or blake3:<hex>")
        return slb_ref.Digest(algos[name], _hex_from(digest, 32))
    for e in td["enumerators"]:
        if e["name"] == node:
            return slb_ref.Uint(e["no"])
    raise _err("R-TXT-TYPE", "unknown enumerator %r" % node)


def _container_from(td, kind, node, schemas):
    if kind == "list":
        if not isinstance(node, list):
            raise _syntax("array expected")
        return slb_ref.List(tuple(_build(x, td["element"], schemas) for x in node))
    if kind == "map":
        if not isinstance(node, list) or not all(isinstance(p, list) and len(p) == 2 for p in node):
            raise _syntax("array of [key, value] pairs expected")
        key_td = {"kind": td["key"]}
        return slb_ref.Map(tuple((_build(k, key_td, schemas), _build(v, td["value"], schemas)) for k, v in node))
    if kind == "oset":
        if not isinstance(node, list):
            raise _syntax("array expected")
        rows = []
        for row in node:
            if not isinstance(row, _Obj) or [k for k, _ in row] != ["id", "order", "value"]:
                raise _syntax('oset entries are {"id", "order", "value"}')
            eid, order, v = (x for _, x in row)
            if not isinstance(eid, str) or isinstance(eid, _Num):
                raise _syntax("element id text expected")
            rows.append((id_from_text(eid), _hex_from(order), _build(v, td["element"], schemas)))
        return slb_ref.Oset(tuple(rows))
    if not isinstance(node, _Obj):
        raise _syntax("object expected")
    if kind == "variant":
        if len(node) != 1:
            raise _syntax("a variant is an object with exactly one key")
        name, body = node[0]
        for c in td["cases"]:
            if c["name"] == name:
                return slb_ref.Variant(c["no"], _build(body, c["type"], schemas))
        if re.match(r"^\?[0-9]+$", name):
            return slb_ref.Variant(int(name[1:]), _unknown_from(body))
        raise _err("R-TXT-TYPE", "unknown variant case %r" % name)
    fields = []
    by_name = {f["name"]: f for f in td["fields"]}
    for name, body in node:
        if name in by_name:
            fields.append((by_name[name]["no"], _build(body, by_name[name]["type"], schemas)))
        elif re.match(r"^\?[0-9]+$", name):
            fields.append((int(name[1:]), _unknown_from(body)))
        else:
            raise _err("R-TXT-TYPE", "unknown field name %r" % name)
    return slb_ref.Struct(tuple(fields))
