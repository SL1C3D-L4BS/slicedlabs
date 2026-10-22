"""Tests of tools/runtime/contract_lock.py (WO-100; RT-INV-GATE-06, SL-ARCH-003 ARCH3-R020).

Each case builds a throwaway root with a copy of SL-ARCH-003, empty header files named by its `layers` table, a
contract dump derived from the document (what the code dumps when the two agree) and a lock written by the tool, then
drifts one value and checks that the error line names it.
"""

import contextlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
_spec = importlib.util.spec_from_file_location("runtime_contract_lock", os.path.join(HERE, "contract_lock.py"))
contract_lock = importlib.util.module_from_spec(_spec)
sys.modules["runtime_contract_lock"] = contract_lock
_spec.loader.exec_module(contract_lock)

PYTHON_FILES = {"../scorecard.json", "hash-matrix.json", "stress-matrix.json"}


class ContractLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="contract-lock-")
        rel, blocks, errors = contract_lock.load_blocks(ROOT)
        self.assertEqual(errors, [])
        os.makedirs(os.path.join(self.tmp, "docs", "arch"))
        self.doc = os.path.join(self.tmp, rel)
        shutil.copyfile(os.path.join(ROOT, rel), self.doc)
        docs, unbound, errors = contract_lock.doc_contract(rel, blocks)
        self.assertEqual(errors, [])
        self.unbound = unbound
        base = os.path.join(self.tmp, *contract_lock.HEADER_DIR.split("/"))
        for row in docs["layers"]:
            for header in row["headers"]:
                path = os.path.join(base, *header.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                open(path, "w", encoding="utf-8").close()
        self.dump = {s: docs[s] for s in contract_lock.DUMP_SECTIONS}
        self.dump["gate_evidence"] = [r for r in docs["gate_evidence"] if r["file"] not in PYTHON_FILES]
        self.dump["format"] = contract_lock.DUMP_FORMAT
        self.dump_path = os.path.join(self.tmp, "contract-dump.json")
        self.lock_path = os.path.join(self.tmp, *contract_lock.LOCK_PATH.split("/"))
        self.write_dump()
        self.assertEqual(self.run_tool("write"), 0)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write_dump(self):
        with open(self.dump_path, "w", encoding="utf-8") as fh:
            json.dump(self.dump, fh, sort_keys=True)

    def run_tool(self, mode):
        with contextlib.redirect_stdout(io.StringIO()):
            return contract_lock.main([mode, self.dump_path, "--root", self.tmp])

    def lines(self):
        return contract_lock.check(self.dump_path, self.tmp, self.lock_path)[0]

    def edit_doc(self, old, new):
        with open(self.doc, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn(old, text)
        with open(self.doc, "w", encoding="utf-8") as fh:
            fh.write(text.replace(old, new, 1))

    def test_rt_inv_gate_06_document_dump_and_lock_agree(self):
        self.assertEqual(self.lines(), [])
        with open(self.lock_path, encoding="utf-8") as fh:
            lock = json.load(fh)
        self.assertEqual(lock["format"], contract_lock.LOCK_FORMAT)
        self.assertEqual(lock["sha256"], contract_lock.contract_sha(lock["contract"]))
        self.assertEqual(sorted(lock["contract"]), sorted(contract_lock.SECTIONS))

    def test_rt_inv_gate_06_a_drifted_errc_number_is_named(self):
        self.edit_doc('{"name": "stale_entity", "number": 6050,', '{"name": "stale_entity", "number": 6059,')
        lines = self.lines()
        self.assertTrue(any(ln.startswith("R-LOCK-DOC") and "errc stale_entity" in ln and "6050" in ln
                            and "6059" in ln for ln in lines), lines)
        self.assertEqual(self.run_tool("check"), 1)

    def test_rt_inv_gate_06_a_drifted_limit_failpoint_and_transition_are_named(self):
        self.edit_doc('{"name": "chunk_bytes", "value": 16384,', '{"name": "chunk_bytes", "value": 8192,')
        self.edit_doc('"name": "alloc.fail"', '"name": "alloc.refuse"')
        self.edit_doc('["Created", "start", "Starting",', '["Created", "start", "Ready",')
        text = "\n".join(self.lines())
        self.assertIn("limit chunk_bytes is 16384 in the code, 8192 in the document", text)
        self.assertIn("failpoints: id RFP0: name", text)
        self.assertIn('rt_states: transition ["Created", "start", "Starting"] is in the code', text)

    def test_rt_inv_gate_06_a_code_change_fails_the_lock_and_names_the_key(self):
        self.dump["limits"]["chunk_bytes"] = 32768
        self.write_dump()
        lines = self.lines()
        self.assertTrue(any(ln.startswith("R-LOCK-LOCK") and "limit chunk_bytes" in ln for ln in lines), lines)
        self.assertTrue(any(ln.startswith("R-LOCK-DOC") and "limit chunk_bytes" in ln for ln in lines), lines)

    def test_rt_inv_gate_06_a_stale_lock_hash_is_r_lock_hash(self):
        with open(self.lock_path, encoding="utf-8") as fh:
            lock = json.load(fh)
        for row in lock["contract"]["error_codes"]:
            if row["name"] == "stale_entity":
                row["number"] = 6059
        with open(self.lock_path, "w", encoding="utf-8") as fh:
            json.dump(lock, fh, indent=2)
        lines = self.lines()
        self.assertTrue(any(ln.startswith("R-LOCK-HASH") for ln in lines), lines)
        self.assertTrue(any(ln.startswith("R-LOCK-LOCK") and "errc stale_entity" in ln for ln in lines), lines)

    def test_rt_inv_gate_06_an_unbound_limit_the_code_defines_is_refused(self):
        name = sorted(self.unbound)[0]
        self.dump["limits"][name] = 1
        self.write_dump()
        self.assertIn('R-LOCK-DOC: limits: limit %s is "bound": false in the document but the code defines it' % name,
                      self.lines())

    def test_rt_inv_gate_06_a_header_outside_the_layers_table_is_named(self):
        path = os.path.join(self.tmp, *contract_lock.HEADER_DIR.split("/"), "planted.hpp")
        open(path, "w", encoding="utf-8").close()
        self.assertTrue(any("header planted.hpp is in the code" in ln for ln in self.lines()))

    def test_rt_inv_gate_06_a_bad_dump_and_usage(self):
        self.dump["format"] = "other/1"
        self.write_dump()
        self.assertTrue(any(ln.startswith("R-LOCK-DUMP") for ln in self.lines()))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(contract_lock.main(["frobnicate"]), 2)
            self.assertEqual(contract_lock.main(["check", "--root"]), 2)


if __name__ == "__main__":
    unittest.main()
