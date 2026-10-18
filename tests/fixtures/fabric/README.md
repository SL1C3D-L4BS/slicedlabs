# tests/fixtures/fabric

Fixtures of the Fabric (Volume 04, SL-ARCH-001 and SL-ARCH-002). `tests/fixtures/MANIFEST.json` lists every file;
change one on purpose with `python3 -I tools/testing/fixtures.py lock`. Text and JSON only.

| Directory | Written by | Holds |
| :---- | :---- | :---- |
| `slb/` | WO-038 | SLB/1 golden corpus: `schemas.json`, `accept.json`, `reject.json` |
| `hash/` | WO-038 | `vectors.json`: SHA-256, id, drbg and object-id vectors (hashlib) |
| `schema/` | WO-040 | schema source and lock fixtures |
| `types/` | WO-045 | generated-type fixtures |
| `tree/` | WO-046 | revision-tree fixtures |
| `tx/` | WO-050 | transaction fixtures |
| `migration/` | WO-057 | migration fixtures |

## slb/ and hash/ (WO-038)

The Python reference (`tools/fabric/slb_ref.py`, `slb_text.py`) is the generator; `tools/fabric/corpus.py` and
`tools/fabric/vectors.py` regenerate every file in memory and compare. `just fabric-golden-check` and
`just fabric-vectors-check` are the gates; `python3 -I tools/fabric/corpus.py write` and
`python3 -I tools/fabric/vectors.py write` regenerate (a builder or the founder after a deliberate format change,
never a gate). The C++ codec (WO-041) must reproduce every `accept.json` hex and oid and reject every
`reject.json` vector with the same rule and offset; WO-037 and WO-041 check their ids and digests against
`hash/vectors.json`.

`accept.json` vectors: `{name, type, value, hex, oid, text}`. `value` is the corpus notation (decimal strings for
integers, hex strings for floats as IEEE bits, bytes, ids and digests):

    false | true | {"uint": "128"} | {"nint": "-1"} | {"f32": "3fc00000"} | {"f64": "3ff8000000000000"}
    {"str": "text"} | {"bytes": "00ff"} | {"id": "<32 hex>"} | {"digest": "<algo 2 hex><64 hex>"}
    {"list": [v...]} | {"map": [[k, v]...]} | {"struct": [[field number, v]...]} | {"variant": [case, v]}
    {"oset": [["<element id 32 hex>", "<order key hex>", v]...]}

Maps, structs and osets are listed in input order (often unsorted); the encoder sorts them. `hex` is the canonical
payload. `oid` is `sha256:<hex>` of the record frame (kind 2, major 1, minor 0, flags 0) around it. `text` is the
canonical text of `type` in `schemas.json` (rules in the `slb_text.py` docstring). A `-0.0` input vector shows the
normalisation: its hex is the `+0.0` bytes.

`reject.json` vectors: `{name, hex, rule, offset, target, policy, type}`. `target` is `value` (decode a payload) or
`frame` (parse an SLFO frame); `policy` is `preserve` or `strict`; `type` names the schema for `strict` (else null).
Checks run in byte order and the first violation wins. Offsets per rule are listed in the `slb_ref.py` docstring.

### Wire layout decisions

ADR-0012 and SL-ARCH-001 fix the tags, varints, canonical ordering and limits; these points were open and are fixed
by the reference (SL-ARCH-001 should absorb them in WO-062):

- list, map, struct and oset are `varint(count)` then the items; a map entry is key value, a struct field is
  `varint(field number)` value, a variant is `varint(case)` value; an oset entry is the 16-byte element id (no tag),
  `varint(len)` order key bytes, and the value. A digest is `algo u8` plus 32 bytes (any algo byte decodes; text
  exists for 1 sha256 and 2 blake3).
- Map keys are uint, nint, string or id values (any other tag is R-SER-TAG at the key); entries are strictly
  ascending by the whole encoded key (so `aa` sorts after `b`, and 16384 before 129). Struct fields are strictly
  ascending by number (numeric, below 2^32); oset entries strictly ascending by (order key bytes, element id) with
  element ids unique over the whole set.
- Strings carry no NUL byte: a NUL is R-SER-UTF8 (SL-ARCH-001 `type_kinds` says "UTF-8, no NUL").
- Depth counts containers only (list, map, struct, variant, oset): 64 nested are accepted, the 65th is R-SER-LIMIT.
  Counts above 2^24 and string, bytes or order-key lengths above 2^24 are R-SER-LIMIT at the count or length varint,
  before any truncation check. A 16 MiB record body cannot be shipped as a vector; the limit is a unit test.
- `R-SER-UNKNOWN` (strict policy, schema directed) is new: SL-ARCH-001 `decode_rules` has no rule for
  `decode_unknown_field`. `R-SER-ENCODE` is encoder-only and never in `reject.json`.
- A negative integer is always `nint` and a non-negative one always `uint`: the "non-negative as nint" violation
  cannot be written, so it has no must-reject vector (the encoder refuses `Nint(5)`; see `test_slb_ref.py`).
- Id text is `<prefix>_<26 upper-case Crockford characters>`; the class byte must match the prefix.

### Rules covered

| Rule | Accept vectors | Must-reject vectors |
| :---- | :---- | :---- |
| ARCH1-R26 tags | one per tag (`bool_*`, `uint_*`, `nint_*`, `f32_*`, `f64_*`, `str_*`, `bytes_*`, `id_*`, `digest_*`, `list_*`, `map_*`, `struct_*`, `variant_*`, `oset_*`) | R-SER-TAG `tag_*`, `map_key_tag_*` |
| ARCH1-R26 varints | `uint_*` at every 7-bit boundary, `nint_*`, `nint_m2p64_wire_min` | R-SER-VARINT `varint_*` |
| ARCH1-R26 canonical order | unsorted-input `map_*`, `struct_point_unsorted_input`, `oset_*` | R-SER-ORDER `order_*`, R-SER-DUP `dup_*` |
| ARCH1-R26 UTF-8 | `str_*` (1 to 4 byte, noncharacters, NFC and NFD) | R-SER-UTF8 `utf8_*` |
| ARCH1-R26 truncation and trailing | | R-SER-TRUNC `trunc_*` (every prefix of three samples), R-SER-TRAILING `trailing_*` |
| ARCH1-R27 floats | `f32_*`, `f64_*` (subnormal, max, `-0.0` input) | R-SER-NAN `nan_*`, R-SER-NEGZERO `negzero_*` |
| ARCH1-R28 unknown fields | `unknown_*`, `variant_unknown_case*` | R-SER-UNKNOWN `unknown_strict_*` |
| ARCH1-R29 text | every vector's `text` | `slb_text` unit tests (non-canonical spellings) |
| ARCH1-R30 limits | `depth_*_63`, `depth_*_64`, `list_128_items` | R-SER-LIMIT `limit_*` |
| ARCH1-R09/R10 frames, object ids | `oid` in every vector; `hash/vectors.json` `oid` section | R-SER-FRAME `frame_*`, frame variants of TRUNC, TRAILING, VARINT, LIMIT |
| ARCH1-R01/R03 ids | `id_class_*`; `hash/vectors.json` `id_text`, `derive`, `well_known`, `imported`, `drbg`, `trie_key` | `id_text_reject` |
