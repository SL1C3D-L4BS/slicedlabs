"""Tests of the contract lock (WO-062). FAB-INV-CF-03: the contract dump equals the sl-data tables of both documents
and the committed contract lock; every planted drift fails naming its rule and the item (an error code by name).

The dump is built here from the documents exactly as sl_fabric_dump_contract writes it, so the tests need no C++
build; `just fabric-contract-lock` runs the check on the real dump.
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import contract_lock  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def make_root(tmp):
    """A mini repository: the real SL-ARCH-001, SL-ARCH-002 and contract lock."""
    os.makedirs(os.path.join(tmp, "docs", "arch"))
    os.makedirs(os.path.join(tmp, "schemas", "fabric"))
    for stem in contract_lock.DOCS:
        name = sorted(n for n in os.listdir(os.path.join(REPO, "docs", "arch")) if n.startswith(stem))[0]
        shutil.copy(os.path.join(REPO, "docs", "arch", name), os.path.join(tmp, "docs", "arch", name))
    shutil.copy(os.path.join(REPO, *contract_lock.LOCK_PATH.split("/")),
                os.path.join(tmp, *contract_lock.LOCK_PATH.split("/")))
    return tmp


def make_dump(root):
    tables, errors = contract_lock.load_docs(root)
    assert not errors, errors
    contract, errors = contract_lock.doc_contract(tables)
    assert not errors, errors
    dump = {"format": contract_lock.DUMP_FORMAT}
    dump.update(json.loads(json.dumps(contract)))
    dump["slb_tags"] = [{"tag": "0x%02X" % r["tag"], "name": r["name"]} for r in contract["slb_tags"]]
    return dump


def doc_path(root, stem):
    arch = os.path.join(root, "docs", "arch")
    return os.path.join(arch, sorted(n for n in os.listdir(arch) if n.startswith(stem))[0])


class ContractLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = make_root(self.tmp)
        self.dump = make_dump(self.root)
        self.lock = os.path.join(self.root, *contract_lock.LOCK_PATH.split("/"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_check(self, dump=None, with_lock=True):
        path = os.path.join(self.tmp, "contract.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.dump if dump is None else dump, fh)
        lines, _ = contract_lock.check(path, self.root, self.lock, with_lock)
        return lines

    def test_fab_inv_cf_03_lock_dump_equals_documents_and_lock(self):
        self.assertEqual(self.run_check(), [])
        self.assertEqual(len(self.dump["error_codes"]), 63)
        self.assertIn("i64", self.dump["type_kinds"])

    def test_fab_inv_cf_03_lock_drifted_error_code_in_a_document_is_named(self):
        path = doc_path(self.root, "SL-ARCH-001-")
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn('["store_io", 5013]', text)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text.replace('["store_io", 5013]', '["store_io", 5016]'))
        lines = self.run_check()
        self.assertTrue(any("R-LOCK-DOC" in line and "errc store_io" in line and "5016" in line for line in lines),
                        lines)

    def test_fab_inv_cf_03_lock_code_drift_fails_documents_and_lock(self):
        dump = json.loads(json.dumps(self.dump))
        dump["tx_states"][0] = "Opened"
        dump["error_codes"].append(["fabric_new_code", 5121])
        lines = self.run_check(dump)
        self.assertTrue(any(line.startswith("R-LOCK-DOC: tx_states") for line in lines), lines)
        self.assertTrue(any(line.startswith("R-LOCK-LOCK: tx_states") for line in lines), lines)
        self.assertTrue(any("errc fabric_new_code" in line and "not in the documents" in line for line in lines), lines)

    def test_fab_inv_cf_03_lock_tampered_lock_fails_its_hash(self):
        with open(self.lock, encoding="utf-8") as fh:
            lock = json.load(fh)
        lock["contract"]["limits"]["depth"] = 65
        with open(self.lock, "w", encoding="utf-8") as fh:
            json.dump(lock, fh)
        lines = self.run_check()
        self.assertTrue(any(line.startswith("R-LOCK-HASH") for line in lines), lines)
        self.assertTrue(any(line.startswith("R-LOCK-LOCK: limits") for line in lines), lines)

    def test_fab_inv_cf_03_lock_write_round_trips(self):
        os.remove(self.lock)
        self.assertTrue(any(line.startswith("R-LOCK-LOCK") for line in self.run_check()))
        path = os.path.join(self.tmp, "contract.json")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = contract_lock.main(["write", path, "--root", self.root])
        self.assertEqual(code, 0, out.getvalue())
        self.assertEqual(self.run_check(), [])
        with open(self.lock, encoding="utf-8") as fh, open(os.path.join(REPO, *contract_lock.LOCK_PATH.split("/")),
                                                            encoding="utf-8") as ref:
            self.assertEqual(json.load(fh), json.load(ref))

    def test_fab_inv_cf_03_lock_bad_dump_and_usage(self):
        self.assertTrue(self.run_check({"format": "other"})[0].startswith("R-LOCK-DUMP"))
        dump = dict(self.dump)
        del dump["failpoints"]
        self.assertIn("failpoints", self.run_check(dump)[0])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(contract_lock.main(["verify", "x"]), 2)


if __name__ == "__main__":
    unittest.main()
