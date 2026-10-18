#!/usr/bin/env python3
"""Fabric hash and identity vectors (WO-038, ADR-0011, ARCH1-R01/R03/R09/R10/R51). Stdlib only (ADR-0004).

Run with `python3 -I tools/fabric/vectors.py check|write [--file PATH]`
(default tests/fixtures/fabric/hash/vectors.json). Every value comes from hashlib, never from the C++ code:
the C++ digest and id code (WO-037, WO-041) is checked against this file.

Sections of vectors.json (one entry per line; text JSON, never empty):
  sha256      {name, input, sha256}  input: {"ascii": s} | {"hex": h} | {"repeat": {"hex": byte, "count": n}}
  id_text     {name, hex, text}      record id bytes and the text form (slb_text.id_to_text)
  id_text_reject  {name, text}       texts every id parser must refuse (R-TXT-SYNTAX)
  derive      {name, class, parent, key_hex, id, text}
                id = [class, 2] + SHA-256("slf-derive/1" || class u8 || parent 16 bytes || u32le len(key) || key)[0:14]
  well_known  {name, class, ordinal, id, text}   [class, 3] + 10 zero bytes + ordinal u32 big-endian
  imported    {name, class, system_hex, external_hex, id, text}
                id = [class, 4] + SHA-256("slf-import/1" || class u8 || u32le len(system) || system
                                          || u32le len(ext) || ext)[0:14]
  drbg        {name, seed, stream_hex, blocks}  block i = SHA-256("slf-drbg/1" || seed u64le
                                                || u32le len(stream) || stream || i u64le)
  trie_key    {name, id, key}        SHA-256("slf-key/1" || id)[0:16] (ARCH1-R51)
  oid         {name, major, minor, kind, flags, payload, frame, preimage, oid, parses, read_only}
                preimage = "slf-obj/1" 0x00 || major || minor || kind || flags || varint(len) || payload
                oid = "sha256:" + SHA-256(preimage); parses is true when the frame is a valid SLFO 1.x frame
Findings: R-VEC-NOFILE, R-VEC-EMPTY, R-VEC-SHAPE, R-VEC-DRIFT, R-VEC-SELF.
"""

import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
import slb_text  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_FILE = os.path.join(ROOT, "tests", "fixtures", "fabric", "hash", "vectors.json")
FORMAT = "slb-hash-vectors/1"
SECTIONS = ("sha256", "id_text", "id_text_reject", "derive", "well_known", "imported", "drbg", "trie_key", "oid")

# FIPS 180-4 / NIST example digests, held here as literals so a broken hashlib or encoder cannot hide.
NIST = {
    b"": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    b"abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq": (
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    ),
    b"a" * 1000000: "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
}


def sha(data):
    return hashlib.sha256(data).digest()


def u32le(n):
    return struct.pack("<I", n)


def mkid(cls, scheme, payload):
    return bytes([cls, scheme]) + payload


def _sha_inputs():
    named = [
        ("empty", b""),
        ("abc", b"abc"),
        ("nist_448_bits", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        (
            "nist_896_bits",
            b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
            b"nopqrstu",
        ),
        ("quick_brown_fox", b"The quick brown fox jumps over the lazy dog"),
        ("one_zero_byte", b"\x00"),
        ("all_byte_values", bytes(range(256))),
        ("object_prefix", b"slf-obj/1\x00"),
    ]
    out = []
    for name, data in named:
        printable = data and all(0x20 <= b < 0x7F for b in data)
        spec = {"ascii": data.decode("ascii")} if printable else {"hex": data.hex()}
        out.append({"name": name, "input": spec, "sha256": sha(data).hex()})
    for count in (55, 56, 57, 63, 64, 65, 119, 120, 128, 1000, 1000000):
        spec = {"repeat": {"hex": "61", "count": count}}
        out.append({"name": "a_x%d" % count, "input": spec, "sha256": sha(b"a" * count).hex()})
    return out


def _id_entry(name, raw, **extra):
    entry = {"name": name}
    entry.update(extra)
    entry["id"] = raw.hex()
    entry["text"] = slb_text.id_to_text(raw)
    return entry


def build():
    doc = {"format": FORMAT}
    doc["sha256"] = _sha_inputs()

    doc["id_text"] = []
    for cls, prefix in sorted(slb_text.ID_CLASSES.items()):
        raw = mkid(cls, 1 + cls % 4, bytes((cls * 7 + i * 13) & 0xFF for i in range(14)))
        name = "class_%02d_%s" % (cls, prefix)
        doc["id_text"].append({"name": name, "hex": raw.hex(), "text": slb_text.id_to_text(raw)})
    doc["id_text"].append({"name": "all_ff_payload", "hex": mkid(15, 4, b"\xff" * 14).hex(),
                           "text": slb_text.id_to_text(mkid(15, 4, b"\xff" * 14))})
    doc["id_text"].append({"name": "zero_payload", "hex": mkid(1, 1, b"\x00" * 14).hex(),
                           "text": slb_text.id_to_text(mkid(1, 1, b"\x00" * 14))})

    good = slb_text.id_to_text(mkid(4, 1, bytes(range(14))))
    body = good.partition("_")[2]
    bad = [
        ("unknown_prefix", "xyz_" + body),
        ("prefix_class_mismatch", "prj_" + body),
        ("no_separator", "ast" + body),
        ("length_25", good[:-1]),
        ("length_27", good + "0"),
        ("lowercase_letter", good[:6] + good[6:].lower()),
        ("excluded_letter_u", good[:-1] + "U"),
        ("excluded_letter_i", good[:-1] + "I"),
        ("excluded_letter_l", good[:-1] + "L"),
        ("excluded_letter_o", good[:-1] + "O"),
        ("first_char_above_7", "ast_8" + body[1:]),
        ("first_char_z", "ast_Z" + body[1:]),
        ("empty", ""),
        ("prefix_only", "ast_"),
    ]
    doc["id_text_reject"] = [{"name": n, "text": t} for n, t in bad]

    root_ns = mkid(13, 3, b"\x00" * 10 + (1).to_bytes(4, "big")[:4])
    doc["derive"] = []
    cases = [
        ("type_in_root_namespace", 3, root_ns, b"game.Transform"),
        ("instance_of_entity", 6, mkid(5, 1, bytes(range(14))), mkid(5, 1, bytes(range(14, 28)))),
        ("empty_key", 7, mkid(4, 1, bytes(range(1, 15))), b""),
        ("utf8_key", 7, mkid(4, 1, bytes(range(1, 15))), "résumé/€".encode("utf-8")),
        ("binary_key", 7, mkid(4, 1, bytes(range(1, 15))), bytes(range(256))),
        ("long_key", 7, mkid(4, 1, bytes(range(1, 15))), b"k" * 300),
        ("same_key_other_class", 6, mkid(4, 1, bytes(range(1, 15))), b""),
        ("same_key_other_parent", 7, mkid(4, 1, bytes(range(2, 16))), b""),
    ]
    for name, cls, parent, key in cases:
        payload = sha(b"slf-derive/1" + bytes([cls]) + parent + u32le(len(key)) + key)[:14]
        entry = _id_entry(name, mkid(cls, 2, payload), **{"class": cls, "parent": parent.hex(), "key_hex": key.hex()})
        doc["derive"].append(_reorder(entry, ("name", "class", "parent", "key_hex", "id", "text")))

    doc["well_known"] = []
    for name, cls, ordinal in (
        ("project_root_namespace", 13, 1),
        ("project_manifest_record", 5, 1),
        ("namespace_ordinal_2", 13, 2),
        ("entity_ordinal_255", 5, 255),
        ("entity_ordinal_65536", 5, 65536),
        ("entity_ordinal_u32_max", 5, 2**32 - 1),
    ):
        raw = mkid(cls, 3, b"\x00" * 10 + ordinal.to_bytes(4, "big"))
        entry = _id_entry(name, raw, **{"class": cls, "ordinal": ordinal})
        doc["well_known"].append(_reorder(entry, ("name", "class", "ordinal", "id", "text")))

    doc["imported"] = []
    imports = [
        ("unity_prefab", 15, b"unity", b"Assets/Hero.prefab"),
        ("empty_system", 15, b"", b"x"),
        ("empty_external", 15, b"blender", b""),
        ("utf8_names", 15, "maï".encode("utf-8"), "☃/\U0001f600".encode("utf-8")),
        ("asset_class", 4, b"unity", b"Assets/Hero.prefab"),
        ("long_external", 15, b"fbx", b"p/" * 200),
    ]
    for name, cls, system, ext in imports:
        data = b"slf-import/1" + bytes([cls]) + u32le(len(system)) + system + u32le(len(ext)) + ext
        raw = mkid(cls, 4, sha(data)[:14])
        entry = _id_entry(name, raw, **{"class": cls, "system_hex": system.hex(), "external_hex": ext.hex()})
        doc["imported"].append(_reorder(entry, ("name", "class", "system_hex", "external_hex", "id", "text")))

    doc["drbg"] = []
    for name, seed, stream in (
        ("seed0_empty_stream", 0, b""),
        ("seed1_ids", 1, b"ids"),
        ("seed_max_entities", 2**64 - 1, b"entity-ids"),
        ("seed42_utf8_stream", 42, "stréam".encode("utf-8")),
    ):
        blocks = [
            sha(b"slf-drbg/1" + struct.pack("<Q", seed) + u32le(len(stream)) + stream + struct.pack("<Q", i)).hex()
            for i in range(4)
        ]
        doc["drbg"].append({"name": name, "seed": str(seed), "stream_hex": stream.hex(), "blocks": blocks})

    doc["trie_key"] = []
    for i in range(4):
        raw = mkid(5, 1, bytes((i * 31 + j) & 0xFF for j in range(14)))
        doc["trie_key"].append({"name": "key_%d" % i, "id": raw.hex(), "key": sha(b"slf-key/1" + raw)[:16].hex()})

    doc["oid"] = _oid_vectors()
    return doc


def _reorder(entry, order):
    return {k: entry[k] for k in order}


def _oid_vectors():
    out = []

    def add(name, kind, payload, major=1, minor=0, flags=0):
        frame = slb_ref.frame(kind, payload, major, minor, flags)
        preimage = slb_ref.oid_preimage(kind, payload, major, minor, flags)
        parses = major == 1 and flags == 0
        out.append(
            {
                "name": name,
                "major": major,
                "minor": minor,
                "kind": kind,
                "flags": flags,
                "payload": payload.hex(),
                "frame": frame.hex(),
                "preimage": preimage.hex(),
                "oid": slb_ref.oid_text(slb_ref.oid(kind, payload, major, minor, flags)),
                "parses": parses,
                "read_only": (minor > 0) if parses else None,
            }
        )

    sample = slb_ref.encode(slb_ref.Struct(((1, slb_ref.Uint(1)), (2, slb_ref.Str("sample")))))
    for kind in range(1, 11):
        add("kind_%02d" % kind, kind, sample)
    add("empty_payload", 2, b"")
    add("length_127", 1, b"\x5a" * 127)
    add("length_128", 1, b"\x5a" * 128)
    add("length_300", 1, b"\x5a" * 300)
    add("minor_1_read_only", 2, sample, minor=1)
    add("minor_255_read_only", 2, sample, minor=255)
    add("major_2_not_openable", 2, sample, major=2)
    add("flags_1_reserved", 2, sample, flags=1)
    add("flags_80_reserved", 2, sample, flags=0x80)
    return out


def render(doc):
    lines = ["{", '"format": "%s",' % FORMAT]
    for i, section in enumerate(SECTIONS):
        rows = ",\n".join(json.dumps(e, ensure_ascii=False) for e in doc[section])
        lines.append('"%s": [\n%s\n]%s' % (section, rows, "," if i < len(SECTIONS) - 1 else ""))
    lines.append("}")
    return "\n".join(lines) + "\n"


def _self_check(doc, findings):
    """The generated data against the literals in NIST and against an independent re-derivation."""
    by_name = {v["name"]: v for v in doc["sha256"]}
    for data, want in NIST.items():
        got = sha(data).hex()
        if got != want:
            findings.append("R-VEC-SELF hashlib: SHA-256 of %d bytes is %s, expected %s" % (len(data), got, want))
    for entry in doc["sha256"]:
        text = entry["input"]
        if "repeat" in text:
            data = bytes.fromhex(text["repeat"]["hex"]) * text["repeat"]["count"]
        elif "hex" in text:
            data = bytes.fromhex(text["hex"])
        else:
            data = text["ascii"].encode("latin-1")
        if sha(data).hex() != entry["sha256"]:
            findings.append("R-VEC-SELF sha256: entry '%s': digest does not match its input" % entry["name"])
    if by_name["abc"]["sha256"] != NIST[b"abc"]:
        findings.append("R-VEC-SELF sha256: entry 'abc' is not the FIPS 180-4 digest")
    seen = {}
    for entry in doc["oid"]:
        if entry["oid"] in seen:
            findings.append("R-VEC-SELF oid: entries '%s' and '%s' share an oid" % (seen[entry["oid"]], entry["name"]))
        seen[entry["oid"]] = entry["name"]


def check(path):
    findings = []
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        return ["R-VEC-NOFILE %s: missing; run `python3 -I tools/fabric/vectors.py write`" % _rel(path)]
    if not text.strip():
        return ["R-VEC-EMPTY %s: the file is empty" % _rel(path)]
    try:
        on_disk = json.loads(text)
    except ValueError as err:
        return ["R-VEC-SHAPE %s: not valid JSON (%s)" % (_rel(path), err)]
    doc = build()
    _self_check(doc, findings)
    if not isinstance(on_disk, dict) or on_disk.get("format") != FORMAT:
        findings.append('R-VEC-SHAPE %s: expected "format": "%s"' % (_rel(path), FORMAT))
        return findings
    for section in SECTIONS:
        want, got = doc[section], on_disk.get(section)
        if not isinstance(got, list) or not got:
            findings.append("R-VEC-SHAPE %s: section '%s' is missing or empty" % (_rel(path), section))
            continue
        got_by_name = {e.get("name"): e for e in got if isinstance(e, dict)}
        where = "%s: section '%s'" % (_rel(path), section)
        for entry in want:
            other = got_by_name.get(entry["name"])
            if other is None:
                findings.append("R-VEC-DRIFT %s: entry '%s' is missing" % (where, entry["name"]))
            elif other != entry:
                key = next(k for k in entry if entry[k] != other.get(k))
                note = "entry '%s': field '%s' differs from hashlib" % (entry["name"], key)
                findings.append("R-VEC-DRIFT %s: %s" % (where, note))
        known = {e["name"] for e in want}
        for name in got_by_name:
            if name not in known:
                findings.append("R-VEC-DRIFT %s: entry '%s' is not produced by the generator" % (where, name))
    if not findings and text != render(doc):
        findings.append("R-VEC-DRIFT %s: same entries but not the canonical rendering" % _rel(path))
    if not findings:
        count = sum(len(doc[s]) for s in SECTIONS)
        print("fabric-vectors-check: ok (%d vectors in %d sections)" % (count, len(SECTIONS)))
    return findings


def _rel(path):
    try:
        return os.path.relpath(path, ROOT).replace(os.sep, "/")
    except ValueError:
        return path


def write(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(render(build()))
    return []


def main(argv):
    args = list(argv)
    path = DEFAULT_FILE
    if "--file" in args:
        i = args.index("--file")
        if i + 1 >= len(args):
            print("usage: vectors.py check|write [--file PATH]", file=sys.stderr)
            return 2
        path = os.path.abspath(args[i + 1])
        del args[i : i + 2]
    if len(args) != 1 or args[0] not in ("check", "write"):
        print("usage: vectors.py check|write [--file PATH]", file=sys.stderr)
        return 2
    findings = check(path) if args[0] == "check" else write(path)
    for line in findings:
        print(line, file=sys.stderr)
    if findings:
        print("fabric-vectors-%s: FAIL (%d finding(s))" % (args[0], len(findings)), file=sys.stderr)
        return 1
    if args[0] == "write":
        print("fabric-vectors-write: wrote %s" % _rel(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
