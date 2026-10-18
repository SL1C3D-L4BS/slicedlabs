"""Tests of the hash and identity vectors (WO-038): hashlib drift detection and the formulas of ADR-0011."""

import contextlib
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slb_ref  # noqa: E402
import slb_text  # noqa: E402
import vectors  # noqa: E402
from slb_ref import SlbError  # noqa: E402


def quiet(fn, *args):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args)


def sha(data):
    return hashlib.sha256(data).digest()


class CommittedVectorsTest(unittest.TestCase):
    def setUp(self):
        with open(vectors.DEFAULT_FILE, encoding="utf-8") as handle:
            self.doc = json.load(handle)

    def test_committed_file_matches_hashlib(self):
        self.assertEqual(quiet(vectors.check, vectors.DEFAULT_FILE), [])

    def test_file_is_text_and_not_empty(self):
        self.assertGreater(os.path.getsize(vectors.DEFAULT_FILE), 1000)
        with open(vectors.DEFAULT_FILE, "rb") as handle:
            handle.read().decode("utf-8")

    def test_every_section_has_entries(self):
        for section in vectors.SECTIONS:
            self.assertGreater(len(self.doc[section]), 0, section)

    def test_sha256_known_answers(self):
        by_name = {v["name"]: v["sha256"] for v in self.doc["sha256"]}
        self.assertEqual(by_name["empty"], "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        self.assertEqual(by_name["abc"], "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        self.assertEqual(by_name["a_x1000000"], "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")

    def test_derive_formula(self):
        for e in self.doc["derive"]:
            parent, key = bytes.fromhex(e["parent"]), bytes.fromhex(e["key_hex"])
            pre = b"slf-derive/1" + bytes([e["class"]]) + parent + struct.pack("<I", len(key)) + key
            self.assertEqual(bytes.fromhex(e["id"]), bytes([e["class"], 2]) + sha(pre)[:14], e["name"])
            self.assertEqual(slb_text.id_from_text(e["text"]), bytes.fromhex(e["id"]))

    def test_imported_formula(self):
        for e in self.doc["imported"]:
            system, ext = bytes.fromhex(e["system_hex"]), bytes.fromhex(e["external_hex"])
            pre = b"slf-import/1" + bytes([e["class"]])
            pre += struct.pack("<I", len(system)) + system + struct.pack("<I", len(ext)) + ext
            self.assertEqual(bytes.fromhex(e["id"]), bytes([e["class"], 4]) + sha(pre)[:14], e["name"])

    def test_well_known_layout(self):
        for e in self.doc["well_known"]:
            raw = bytes.fromhex(e["id"])
            self.assertEqual(raw[:2], bytes([e["class"], 3]))
            self.assertEqual(raw[2:12], b"\x00" * 10)
            self.assertEqual(int.from_bytes(raw[12:], "big"), e["ordinal"])

    def test_drbg_blocks(self):
        for e in self.doc["drbg"]:
            stream = bytes.fromhex(e["stream_hex"])
            for i, block in enumerate(e["blocks"]):
                pre = b"slf-drbg/1" + struct.pack("<Q", int(e["seed"])) + struct.pack("<I", len(stream)) + stream
                self.assertEqual(block, sha(pre + struct.pack("<Q", i)).hex(), e["name"])

    def test_oid_preimage_and_frames(self):
        for e in self.doc["oid"]:
            payload = bytes.fromhex(e["payload"])
            pre = b"slf-obj/1\x00" + bytes([e["major"], e["minor"], e["kind"], e["flags"]])
            pre += slb_ref.varint(len(payload)) + payload
            self.assertEqual(bytes.fromhex(e["preimage"]), pre)
            self.assertEqual(e["oid"], "sha256:" + sha(pre).hex())
            if e["parses"]:
                f = slb_ref.parse_frame(bytes.fromhex(e["frame"]))
                got = (f.kind, f.minor, f.payload, f.read_only)
                self.assertEqual(got, (e["kind"], e["minor"], payload, e["minor"] > 0))
            else:
                with self.assertRaises(SlbError):
                    slb_ref.parse_frame(bytes.fromhex(e["frame"]))
        self.assertEqual(len({e["oid"] for e in self.doc["oid"]}), len(self.doc["oid"]))

    def test_trie_key(self):
        for e in self.doc["trie_key"]:
            self.assertEqual(e["key"], sha(b"slf-key/1" + bytes.fromhex(e["id"]))[:16].hex())

    def test_id_text_vectors(self):
        for e in self.doc["id_text"]:
            self.assertEqual(slb_text.id_to_text(bytes.fromhex(e["hex"])), e["text"])
            self.assertEqual(slb_text.id_from_text(e["text"]), bytes.fromhex(e["hex"]))
        for e in self.doc["id_text_reject"]:
            with self.assertRaises(SlbError, msg=e["name"]):
                slb_text.id_from_text(e["text"])


class PlantedDriftTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self._tmp.name, "vectors.json")
        shutil.copy(vectors.DEFAULT_FILE, self.path)

    def tearDown(self):
        self._tmp.cleanup()

    def plant(self, section, name, field, value):
        with open(self.path, encoding="utf-8") as handle:
            doc = json.load(handle)
        next(e for e in doc[section] if e["name"] == name)[field] = value
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(vectors.render(doc))

    def test_control_is_clean(self):
        self.assertEqual(quiet(vectors.check, self.path), [])

    def test_changed_digest_is_named(self):
        self.plant("sha256", "abc", "sha256", "00" * 32)
        out = "\n".join(quiet(vectors.check, self.path))
        self.assertIn("R-VEC-DRIFT", out)
        self.assertIn("section 'sha256': entry 'abc': field 'sha256'", out)

    def test_changed_derived_id_is_named(self):
        self.plant("derive", "empty_key", "id", "00" * 16)
        self.assertIn("entry 'empty_key'", "\n".join(quiet(vectors.check, self.path)))

    def test_changed_oid_is_named(self):
        self.plant("oid", "kind_02", "oid", "sha256:" + "11" * 32)
        self.assertIn("entry 'kind_02'", "\n".join(quiet(vectors.check, self.path)))

    def test_empty_file_is_refused(self):
        open(self.path, "w").close()
        self.assertTrue(quiet(vectors.check, self.path)[0].startswith("R-VEC-EMPTY"))

    def test_missing_file_is_refused(self):
        os.remove(self.path)
        self.assertTrue(quiet(vectors.check, self.path)[0].startswith("R-VEC-NOFILE"))

    def test_write_is_deterministic(self):
        other = os.path.join(self._tmp.name, "again.json")
        vectors.write(other)
        with open(other, "rb") as a, open(vectors.DEFAULT_FILE, "rb") as b:
            self.assertEqual(a.read(), b.read())


if __name__ == "__main__":
    unittest.main()
