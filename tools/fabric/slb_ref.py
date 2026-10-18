"""SLB/1 reference codec (WO-038, SL-ARCH-001 Serialization, ADR-0011, ADR-0012). Stdlib only (ADR-0004).

The independent Python reference that the C++ codec (WO-041) is checked against, byte for byte. Public surface:

  encode(value) -> bytes                      canonical SLB/1 (sorts maps, structs and osets; refuses duplicates)
  decode(data, policy="preserve", schema=None, type_name=None) -> value
                                              rejects every non-canonical form with SlbError(rule, offset)
  frame(kind, payload, major=1, minor=0, flags=0) -> bytes
  parse_frame(data) -> Frame                  magic SLFO, major, minor, kind, flags, varint length, payload
  oid(kind, payload, major=1, minor=0, flags=0) -> 33 bytes {algo u8 = 1, SHA-256}
  value_to_json(v) / value_from_json(j)       the corpus value notation (see tests/fixtures/fabric/README.md)

Value model: bool (Python bool), Uint, Nint, F32, F64, Str, Bytes, Id, Digest, List, Map, Struct, Variant, Oset.

Wire layout (tag byte first; the table in SL-ARCH-001 `slb_tags` fixes the tags, ADR-0012 the rest):
  0x01 false, 0x02 true | 0x03 uint LEB128 | 0x04 nint LEB128(-(v+1)) | 0x05 f32 LE | 0x06 f64 LE
  0x07 string varint(len) UTF-8 | 0x08 bytes varint(len) | 0x09 id 16 bytes | 0x0A digest algo u8 + 32 bytes
  0x0B list varint(count) value* | 0x0C map varint(count) (key value)*, key tag in {uint, nint, string, id}
  0x0D struct varint(count) (varint(field number) value)* | 0x0E variant varint(case) value
  0x0F oset varint(count) (16-byte element id, varint(len) order key, value)*

Offsets in SlbError are byte offsets from the start of the decoded buffer (the payload for decode, the frame for
parse_frame). They are defined per rule, so the C++ decoder can reproduce them:
  R-SER-TAG       the tag byte (also a map key whose tag is not uint, nint, string or id)
  R-SER-VARINT    the first byte of the varint (overlong, or more than 64 bits)
  R-SER-ORDER     the first byte of the out-of-order map key, struct field number or oset entry
  R-SER-DUP       the first byte of the repeated map key, struct field number, oset entry (same order key and
                  element id) or oset element id (same id under another order key)
  R-SER-UTF8      the first content byte of the string (ill-formed UTF-8, or a NUL byte)
  R-SER-NAN       the tag byte of the f32 or f64 (NaN or infinity)
  R-SER-NEGZERO   the tag byte of the f32 or f64 whose bits are the -0.0 pattern
  R-SER-TRUNC     the length of the input (the position where more bytes were needed)
  R-SER-TRAILING  the first byte after the value (or after the frame payload)
  R-SER-LIMIT     depth: the tag byte of the 65th nested container; items and lengths: the count or length varint;
                  field number or variant case >= 2^32: its varint; input or frame payload > 16 MiB: offset 0 / 8
  R-SER-FRAME     bad magic (0), major other than 1 (4), kind outside 1..10 (6), flags other than 0 (7)
  R-SER-UNKNOWN   strict policy only: the field number of a struct field the schema does not declare
Checks run in byte order and the first violation wins; for a map entry the key is decoded, then ordered, then the
value is decoded. R-SER-ENCODE is raised by encode() only (a value the model cannot represent).
"""

import hashlib
import math
import struct
from dataclasses import dataclass

MAX_DEPTH = 64
MAX_ITEMS = 1 << 24
MAX_BODY = 1 << 24
MAX_U64 = (1 << 64) - 1
MAX_U32 = (1 << 32) - 1
MAGIC = b"SLFO"
MAJOR = 1
OBJECT_KINDS = tuple(range(1, 11))
OID_PREFIX = b"slf-obj/1\x00"
ALGO_SHA256 = 1
POLICIES = ("preserve", "strict")

T_FALSE, T_TRUE, T_UINT, T_NINT, T_F32, T_F64, T_STR, T_BYTES = 1, 2, 3, 4, 5, 6, 7, 8
T_ID, T_DIGEST, T_LIST, T_MAP, T_STRUCT, T_VARIANT, T_OSET = 9, 10, 11, 12, 13, 14, 15
MAP_KEY_TAGS = (T_UINT, T_NINT, T_STR, T_ID)

RULES = (
    "R-SER-TAG",
    "R-SER-VARINT",
    "R-SER-ORDER",
    "R-SER-DUP",
    "R-SER-UTF8",
    "R-SER-NAN",
    "R-SER-NEGZERO",
    "R-SER-TRUNC",
    "R-SER-TRAILING",
    "R-SER-LIMIT",
    "R-SER-FRAME",
    "R-SER-UNKNOWN",
)


class SlbError(Exception):
    """A rejection: `rule` is an R-SER-* id, `offset` the byte offset (None for encoder-side errors)."""

    def __init__(self, rule, offset=None, message=""):
        self.rule = rule
        self.offset = offset
        self.message = message
        text = "%s at offset %s" % (rule, offset) if offset is not None else rule
        super().__init__(text + (": " + message if message else ""))


@dataclass(frozen=True)
class Uint:
    value: int


@dataclass(frozen=True)
class Nint:
    value: int


@dataclass(frozen=True)
class F32:
    value: float


@dataclass(frozen=True)
class F64:
    value: float


@dataclass(frozen=True)
class Str:
    value: str


@dataclass(frozen=True)
class Bytes:
    value: bytes


@dataclass(frozen=True)
class Id:
    value: bytes


@dataclass(frozen=True)
class Digest:
    algo: int
    value: bytes


@dataclass(frozen=True)
class List:
    items: tuple


@dataclass(frozen=True)
class Map:
    entries: tuple  # ((key, value), ...) in any order; encode() sorts


@dataclass(frozen=True)
class Struct:
    fields: tuple  # ((field number, value), ...) in any order; encode() sorts


@dataclass(frozen=True)
class Variant:
    case: int
    value: object


@dataclass(frozen=True)
class Oset:
    entries: tuple  # ((element id bytes, order key bytes, value), ...) in any order; encode() sorts


@dataclass(frozen=True)
class Frame:
    major: int
    minor: int
    kind: int
    flags: int
    payload: bytes
    read_only: bool  # a newer minor opens read-only (ARCH1-R10)


def _enc_error(message):
    return SlbError("R-SER-ENCODE", None, message)


# ---- varint -------------------------------------------------------------------------------------------------

def varint(n):
    """Shortest LEB128 of 0 <= n < 2^64."""
    if not isinstance(n, int) or isinstance(n, bool) or n < 0 or n > MAX_U64:
        raise _enc_error("varint out of range: %r" % (n,))
    out = bytearray()
    while True:
        low = n & 0x7F
        n >>= 7
        if n:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


# ---- encoder ------------------------------------------------------------------------------------------------

def _float_bits_ok(x, width):
    if not isinstance(x, float):
        raise _enc_error("float value must be a Python float")
    if math.isnan(x) or math.isinf(x):
        raise SlbError("R-SER-NAN", None, "non-finite float")
    if x == 0.0:
        return 0.0  # -0.0 is written as +0.0 (ARCH1-R27)
    if width == 4:
        try:
            if struct.unpack("<f", struct.pack("<f", x))[0] != x:
                raise _enc_error("not exactly representable as f32: %r" % (x,))
        except OverflowError:
            raise _enc_error("out of f32 range: %r" % (x,)) from None
    return x


def _enc(v, out, depth):
    if v is False:
        out.append(T_FALSE)
    elif v is True:
        out.append(T_TRUE)
    elif isinstance(v, Uint):
        out.append(T_UINT)
        out += varint(v.value)
    elif isinstance(v, Nint):
        if not isinstance(v.value, int) or v.value >= 0:
            raise _enc_error("nint must be negative: %r" % (v.value,))
        out.append(T_NINT)
        out += varint(-(v.value + 1))
    elif isinstance(v, F32):
        out.append(T_F32)
        out += struct.pack("<f", _float_bits_ok(v.value, 4))
    elif isinstance(v, F64):
        out.append(T_F64)
        out += struct.pack("<d", _float_bits_ok(v.value, 8))
    elif isinstance(v, Str):
        try:
            raw = v.value.encode("utf-8")
        except UnicodeEncodeError:
            raise SlbError("R-SER-UTF8", None, "string is not valid Unicode") from None
        if b"\x00" in raw:
            raise SlbError("R-SER-UTF8", None, "NUL in string")
        _enc_blob(out, T_STR, raw)
    elif isinstance(v, Bytes):
        _enc_blob(out, T_BYTES, bytes(v.value))
    elif isinstance(v, Id):
        if len(v.value) != 16:
            raise _enc_error("id must be 16 bytes")
        out.append(T_ID)
        out += v.value
    elif isinstance(v, Digest):
        if len(v.value) != 32 or not 0 <= v.algo <= 255:
            raise _enc_error("digest is an algo byte and 32 bytes")
        out.append(T_DIGEST)
        out.append(v.algo)
        out += v.value
    elif isinstance(v, (List, Map, Struct, Variant, Oset)):
        _enc_container(v, out, depth)
    else:
        raise _enc_error("not an SLB/1 value: %r" % (type(v).__name__,))


def _enc_blob(out, tag, raw):
    if len(raw) > MAX_BODY:
        raise SlbError("R-SER-LIMIT", None, "length over 2^24")
    out.append(tag)
    out += varint(len(raw))
    out += raw


def _count(n):
    if n > MAX_ITEMS:
        raise SlbError("R-SER-LIMIT", None, "more than 2^24 items")
    return varint(n)


def _enc_container(v, out, depth):
    depth += 1
    if depth > MAX_DEPTH:
        raise SlbError("R-SER-LIMIT", None, "depth over 64")
    if isinstance(v, List):
        out.append(T_LIST)
        out += _count(len(v.items))
        for item in v.items:
            _enc(item, out, depth)
    elif isinstance(v, Map):
        out.append(T_MAP)
        out += _count(len(v.entries))
        keyed = []
        for key, value in v.entries:
            kb = bytearray()
            _enc(key, kb, depth)
            if kb[0] not in MAP_KEY_TAGS:
                raise SlbError("R-SER-TAG", None, "map key tag %#04x" % kb[0])
            keyed.append((bytes(kb), value))
        keyed.sort(key=lambda e: e[0])
        for i in range(1, len(keyed)):
            if keyed[i][0] == keyed[i - 1][0]:
                raise SlbError("R-SER-DUP", None, "duplicate map key")
        for kb, value in keyed:
            out += kb
            _enc(value, out, depth)
    elif isinstance(v, Struct):
        out.append(T_STRUCT)
        out += _count(len(v.fields))
        fields = sorted(v.fields, key=lambda f: f[0])
        for i, (no, value) in enumerate(fields):
            if not isinstance(no, int) or not 0 <= no <= MAX_U32:
                raise SlbError("R-SER-LIMIT", None, "field number outside u32")
            if i and no == fields[i - 1][0]:
                raise SlbError("R-SER-DUP", None, "duplicate struct field %d" % no)
            out += varint(no)
            _enc(value, out, depth)
    elif isinstance(v, Variant):
        if not isinstance(v.case, int) or not 0 <= v.case <= MAX_U32:
            raise SlbError("R-SER-LIMIT", None, "variant case outside u32")
        out.append(T_VARIANT)
        out += varint(v.case)
        _enc(v.value, out, depth)
    else:
        out.append(T_OSET)
        out += _count(len(v.entries))
        entries = sorted(v.entries, key=lambda e: (e[1], e[0]))
        seen = set()
        for eid, key, value in entries:
            if len(eid) != 16:
                raise _enc_error("oset element id must be 16 bytes")
            if eid in seen:
                raise SlbError("R-SER-DUP", None, "duplicate oset element id")
            seen.add(eid)
            if len(key) > MAX_BODY:
                raise SlbError("R-SER-LIMIT", None, "order key over 2^24 bytes")
            out += eid
            out += varint(len(key))
            out += key
            _enc(value, out, depth)


def encode(value):
    out = bytearray()
    _enc(value, out, 0)
    if len(out) > MAX_BODY:
        raise SlbError("R-SER-LIMIT", None, "record body over 16 MiB")
    return bytes(out)


# ---- decoder ------------------------------------------------------------------------------------------------

class _Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def byte(self):
        if self.pos >= len(self.data):
            raise SlbError("R-SER-TRUNC", len(self.data))
        b = self.data[self.pos]
        self.pos += 1
        return b

    def take(self, n):
        if self.pos + n > len(self.data):
            raise SlbError("R-SER-TRUNC", len(self.data))
        raw = self.data[self.pos : self.pos + n]
        self.pos += n
        return raw

    def varint(self):
        start = self.pos
        result = 0
        for i in range(10):
            b = self.byte()
            if i == 9 and b > 1:
                raise SlbError("R-SER-VARINT", start, "more than 64 bits")
            result |= (b & 0x7F) << (7 * i)
            if not b & 0x80:
                if b == 0 and i > 0:
                    raise SlbError("R-SER-VARINT", start, "overlong")
                return result
        raise SlbError("R-SER-VARINT", start, "more than 64 bits")  # pragma: no cover (i == 9 returns or raises)


def _child_td(td, schema, kind, key=None):
    """The schema descriptor of a child, or None when the schema does not describe it."""
    if td is None:
        return None
    try:
        if kind == "list" or kind == "oset":
            sub = td["element"]
        elif kind == "map":
            sub = td["value"]
        elif kind == "variant":
            sub = next(c["type"] for c in td["cases"] if c["no"] == key)
        else:
            sub = next(f["type"] for f in td["fields"] if f["no"] == key)
    except (KeyError, StopIteration, TypeError):
        return None
    return _resolve(schema, sub)


def _resolve(schema, ref):
    if isinstance(ref, str):
        return schema.get(ref) if schema else None
    return ref


def _dec(r, depth, policy, td, schema):
    off = r.pos
    tag = r.byte()
    kind = td.get("kind") if isinstance(td, dict) else None
    if tag == T_FALSE:
        return False
    if tag == T_TRUE:
        return True
    if tag == T_UINT:
        return Uint(r.varint())
    if tag == T_NINT:
        return Nint(-(r.varint() + 1))
    if tag in (T_F32, T_F64):
        return _dec_float(r, tag, off)
    if tag in (T_STR, T_BYTES):
        return _dec_blob(r, tag)
    if tag == T_ID:
        return Id(r.take(16))
    if tag == T_DIGEST:
        algo = r.byte()
        return Digest(algo, r.take(32))
    if tag in (T_LIST, T_MAP, T_STRUCT, T_VARIANT, T_OSET):
        if depth + 1 > MAX_DEPTH:
            raise SlbError("R-SER-LIMIT", off, "depth over 64")
        return _dec_container(r, tag, depth + 1, policy, td if _kind_matches(kind, tag) else None, schema)
    raise SlbError("R-SER-TAG", off, "unknown tag %#04x" % tag)


_KIND_OF_TAG = {T_LIST: "list", T_MAP: "map", T_STRUCT: "struct", T_VARIANT: "variant", T_OSET: "oset"}


def _kind_matches(kind, tag):
    return kind == _KIND_OF_TAG.get(tag)


def _dec_float(r, tag, off):
    raw = r.take(4 if tag == T_F32 else 8)
    bits = int.from_bytes(raw, "little")
    if tag == T_F32:
        if (bits >> 23) & 0xFF == 0xFF:
            raise SlbError("R-SER-NAN", off, "NaN or infinity")
        if bits == 0x80000000:
            raise SlbError("R-SER-NEGZERO", off, "-0.0")
        return F32(struct.unpack("<f", raw)[0])
    if (bits >> 52) & 0x7FF == 0x7FF:
        raise SlbError("R-SER-NAN", off, "NaN or infinity")
    if bits == 0x8000000000000000:
        raise SlbError("R-SER-NEGZERO", off, "-0.0")
    return F64(struct.unpack("<d", raw)[0])


def _dec_blob(r, tag):
    len_off = r.pos
    n = r.varint()
    if n > MAX_BODY:
        raise SlbError("R-SER-LIMIT", len_off, "length over 2^24")
    raw = r.take(n)
    if tag == T_BYTES:
        return Bytes(raw)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise SlbError("R-SER-UTF8", r.pos - n, "ill-formed UTF-8") from None
    if "\x00" in text:
        raise SlbError("R-SER-UTF8", r.pos - n, "NUL in string")
    return Str(text)


def _dec_container(r, tag, depth, policy, td, schema):
    kind = _KIND_OF_TAG[tag]
    if tag == T_VARIANT:
        case_off = r.pos
        case = r.varint()
        if case > MAX_U32:
            raise SlbError("R-SER-LIMIT", case_off, "variant case outside u32")
        sub = _child_td(td, schema, kind, case)
        return Variant(case, _dec(r, depth, policy, sub, schema))
    count_off = r.pos
    n = r.varint()
    if n > MAX_ITEMS:
        raise SlbError("R-SER-LIMIT", count_off, "more than 2^24 items")
    items = []
    prev = None
    seen = set()
    for _ in range(n):
        start = r.pos
        if tag == T_LIST:
            items.append(_dec(r, depth, policy, _child_td(td, schema, kind), schema))
        elif tag == T_MAP:
            if start < len(r.data) and r.data[start] not in MAP_KEY_TAGS:
                raise SlbError("R-SER-TAG", start, "map key tag %#04x" % r.data[start])
            key = _dec(r, depth, policy, None, schema)
            prev = _ordered(prev, bytes(r.data[start : r.pos]), start)
            items.append((key, _dec(r, depth, policy, _child_td(td, schema, kind), schema)))
        elif tag == T_STRUCT:
            no = r.varint()
            if no > MAX_U32:
                raise SlbError("R-SER-LIMIT", start, "field number outside u32")
            prev = _ordered(prev, no, start)
            if policy == "strict" and td is not None and all(f["no"] != no for f in td.get("fields", [])):
                raise SlbError("R-SER-UNKNOWN", start, "field %d is not in the schema" % no)
            items.append((no, _dec(r, depth, policy, _child_td(td, schema, kind, no), schema)))
        else:
            eid = r.take(16)
            klen_off = r.pos
            klen = r.varint()
            if klen > MAX_BODY:
                raise SlbError("R-SER-LIMIT", klen_off, "order key over 2^24 bytes")
            okey = r.take(klen)
            this = (okey, eid)
            if prev is not None and this == prev:
                raise SlbError("R-SER-DUP", start, "duplicate oset entry")
            if prev is not None and this < prev:
                raise SlbError("R-SER-ORDER", start, "oset entry out of order")
            if eid in seen:
                raise SlbError("R-SER-DUP", start, "duplicate oset element id")
            seen.add(eid)
            prev = this
            items.append((eid, okey, _dec(r, depth, policy, _child_td(td, schema, kind), schema)))
    wrap = {T_LIST: List, T_MAP: Map, T_STRUCT: Struct, T_OSET: Oset}[tag]
    return wrap(tuple(items))


def _ordered(prev, this, start):
    if prev is not None and this == prev:
        raise SlbError("R-SER-DUP", start, "duplicate key")
    if prev is not None and this < prev:
        raise SlbError("R-SER-ORDER", start, "key out of order")
    return this


def decode(data, policy="preserve", schema=None, type_name=None):
    """Decode one canonical SLB/1 value that fills `data`. `schema` ({name: descriptor}) and `type_name` only
    matter for policy "strict", which rejects struct fields the schema does not declare (R-SER-UNKNOWN)."""
    if policy not in POLICIES:
        raise ValueError("policy must be preserve or strict")
    data = bytes(data)
    if len(data) > MAX_BODY:
        raise SlbError("R-SER-LIMIT", 0, "record body over 16 MiB")
    td = _resolve(schema, type_name) if type_name is not None else None
    r = _Reader(data)
    value = _dec(r, 0, policy, td, schema)
    if r.pos < len(data):
        raise SlbError("R-SER-TRAILING", r.pos, "%d trailing byte(s)" % (len(data) - r.pos))
    return value


# ---- frames and object ids ----------------------------------------------------------------------------------

def frame(kind, payload, major=MAJOR, minor=0, flags=0):
    """magic SLFO, major, minor, kind, flags, varint(len(payload)), payload (ARCH1-R10)."""
    return MAGIC + bytes([major, minor, kind, flags]) + varint(len(payload)) + bytes(payload)


def oid_preimage(kind, payload, major=MAJOR, minor=0, flags=0):
    """"slf-obj/1" 0x00 || major || minor || kind || flags || varint(length) || payload (ARCH1-R09)."""
    return OID_PREFIX + bytes([major, minor, kind, flags]) + varint(len(payload)) + bytes(payload)


def oid(kind, payload, major=MAJOR, minor=0, flags=0):
    """Object id {algo u8 = 1, SHA-256 of the preimage}: 33 bytes."""
    return bytes([ALGO_SHA256]) + hashlib.sha256(oid_preimage(kind, payload, major, minor, flags)).digest()


def oid_text(o):
    if len(o) != 33 or o[0] != ALGO_SHA256:
        raise _enc_error("object id is algo 1 and 32 bytes")
    return "sha256:" + o[1:].hex()


def frame_oid(data):
    """The object id of a framed object, computed from the frame bytes after parse_frame accepts them."""
    f = parse_frame(data)
    return oid(f.kind, f.payload, f.major, f.minor, f.flags)


def parse_frame(data):
    data = bytes(data)
    avail = min(len(data), 4)
    if data[:avail] != MAGIC[:avail]:
        raise SlbError("R-SER-FRAME", 0, "bad magic")
    if len(data) < 8:
        raise SlbError("R-SER-TRUNC", len(data))
    major, minor, kind, flags = data[4], data[5], data[6], data[7]
    if major != MAJOR:
        raise SlbError("R-SER-FRAME", 4, "unsupported major %d (format_unsupported)" % major)
    if kind not in OBJECT_KINDS:
        raise SlbError("R-SER-FRAME", 6, "unknown object kind %d" % kind)
    if flags != 0:
        raise SlbError("R-SER-FRAME", 7, "reserved flags %#04x" % flags)
    r = _Reader(data)
    r.pos = 8
    n = r.varint()
    if n > MAX_BODY:
        raise SlbError("R-SER-LIMIT", 8, "payload length over 16 MiB")
    payload = r.take(n)
    if r.pos < len(data):
        raise SlbError("R-SER-TRAILING", r.pos, "%d trailing byte(s)" % (len(data) - r.pos))
    return Frame(major, minor, kind, flags, payload, minor > 0)


# ---- corpus value notation ----------------------------------------------------------------------------------

def value_to_json(v):
    """Value -> the JSON-able notation of tests/fixtures/fabric/slb/*.json (decimal strings, hex strings)."""
    if v is True or v is False:
        return v
    if isinstance(v, Uint):
        return {"uint": str(v.value)}
    if isinstance(v, Nint):
        return {"nint": str(v.value)}
    if isinstance(v, F32):
        return {"f32": "%08x" % int.from_bytes(struct.pack("<f", v.value), "little")}
    if isinstance(v, F64):
        return {"f64": "%016x" % int.from_bytes(struct.pack("<d", v.value), "little")}
    if isinstance(v, Str):
        return {"str": v.value}
    if isinstance(v, Bytes):
        return {"bytes": v.value.hex()}
    if isinstance(v, Id):
        return {"id": v.value.hex()}
    if isinstance(v, Digest):
        return {"digest": "%02x%s" % (v.algo, v.value.hex())}
    if isinstance(v, List):
        return {"list": [value_to_json(x) for x in v.items]}
    if isinstance(v, Map):
        return {"map": [[value_to_json(k), value_to_json(x)] for k, x in v.entries]}
    if isinstance(v, Struct):
        return {"struct": [[no, value_to_json(x)] for no, x in v.fields]}
    if isinstance(v, Variant):
        return {"variant": [v.case, value_to_json(v.value)]}
    if isinstance(v, Oset):
        return {"oset": [[e.hex(), k.hex(), value_to_json(x)] for e, k, x in v.entries]}
    raise _enc_error("not an SLB/1 value: %r" % (type(v).__name__,))


def _bits_to_float(text, width):
    raw = int(text, 16).to_bytes(width, "little")
    return struct.unpack("<f" if width == 4 else "<d", raw)[0]


def value_from_json(j):
    if j is True or j is False:
        return j
    if not isinstance(j, dict) or len(j) != 1:
        raise _enc_error("bad value notation: %r" % (j,))
    (name, body), = j.items()
    if name == "uint":
        return Uint(int(body))
    if name == "nint":
        return Nint(int(body))
    if name == "f32":
        return F32(_bits_to_float(body, 4))
    if name == "f64":
        return F64(_bits_to_float(body, 8))
    if name == "str":
        return Str(body)
    if name == "bytes":
        return Bytes(bytes.fromhex(body))
    if name == "id":
        return Id(bytes.fromhex(body))
    if name == "digest":
        raw = bytes.fromhex(body)
        return Digest(raw[0], raw[1:])
    if name == "list":
        return List(tuple(value_from_json(x) for x in body))
    if name == "map":
        return Map(tuple((value_from_json(k), value_from_json(x)) for k, x in body))
    if name == "struct":
        return Struct(tuple((no, value_from_json(x)) for no, x in body))
    if name == "variant":
        return Variant(body[0], value_from_json(body[1]))
    if name == "oset":
        return Oset(tuple((bytes.fromhex(e), bytes.fromhex(k), value_from_json(x)) for e, k, x in body))
    raise _enc_error("unknown value notation key: %r" % (name,))
