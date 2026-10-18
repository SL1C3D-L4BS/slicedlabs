"""Unit tests for vol01-gate: a temp-root fixture, one planted break per rule."""

import copy
import contextlib
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_vol01_gate as gate  # noqa: E402

WO = "WO-001"


def write(root, rel, text):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def doc_path(doc_id):
    if doc_id.startswith("SL-GOV"):
        return "docs/gov/%s-x.md" % doc_id
    if doc_id.startswith("SL-PLAN"):
        return "docs/plan/%s-x.md" % doc_id
    if doc_id.startswith("SL-VOL"):
        return "docs/vol/%s.md" % doc_id
    return "registry/%s-x.json" % doc_id


def build(
    root, contract='{"contracts": [{"id": "C1", "enforced_by": "just real"}, {"id": "C2", "verified_by": "pending"}]}'
):
    docs = [{"id": d, "authoring_wo": WO} for d in gate.EXPECTED_IDS]
    eps = [{"episode": e, "document": gate.EXPECTED_IDS[0], "wo": WO} for e in gate.EXPECTED_EPISODES]
    write(root, gate.REL, json.dumps({"schema": gate.SCHEMA, "documents": docs, "episodes": eps}))
    reg = [{"id": d, "path": doc_path(d), "authoring_wo": WO} for d in gate.EXPECTED_IDS]
    write(root, gate.REG_REL, json.dumps({"schema": "sl-reg-001/1", "documents": reg}))
    for d in gate.EXPECTED_IDS:
        p = doc_path(d)
        if d == "SL-PLAN-001":
            body = "plan\n"
            write(root, p, body)
            write(root, gate.PLAN_SHA_REL, hashlib.sha256(body.encode()).hexdigest() + "\n")
        elif p.endswith(".json"):
            write(root, p, "{}")
        else:
            text = "---\nid: %s\nstatus: Draft\n---\n# T\n" % d
            if d == "SL-GOV-008":
                text += "\n```sl-data\n%s\n```\n" % contract
            write(root, p, text)
    write(root, "work-orders/%s.md" % WO, "---\nid: %s\n---\n" % WO)
    write(root, "justfile", "real:\n    @true\n\nstubbed:\n    @just stub stubbed\n")


def rules(errors):
    return {e.split(": ")[1] for e in errors}


class Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        build(self.root)

    def tearDown(self):
        self._tmp.cleanup()

    def mutate(self, fn):
        path = os.path.join(self.root, *gate.REL.split("/"))
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        fn(data)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh)

    def run_gate(self):
        return gate.gate(self.root)

    def test_clean(self):
        errors, rep = self.run_gate()
        self.assertEqual(errors, [])
        self.assertEqual(rep["contracts"], 2)
        self.assertEqual(len(rep["pending"]), 1)

    def test_ids_missing_extra_duplicate(self):
        self.mutate(lambda d: d["documents"].pop(0))
        self.assertIn("R-V1-IDS", rules(self.run_gate()[0]))
        self.setUp()
        self.mutate(lambda d: d["documents"].append({"id": "SL-GOV-099", "authoring_wo": WO}))
        self.assertIn("R-V1-IDS", rules(self.run_gate()[0]))
        self.setUp()
        self.mutate(lambda d: d["documents"].append(copy.deepcopy(d["documents"][0])))
        errors = self.run_gate()[0]
        self.assertTrue(any("listed 2 times" in e for e in errors))

    def test_file_missing_front_matter_and_plan_hash(self):
        os.remove(os.path.join(self.root, doc_path("SL-GOV-002")))
        write(self.root, doc_path("SL-GOV-003"), "no front matter\n")
        write(self.root, doc_path("SL-GOV-004"), "---\nid: SL-GOV-005\nstatus: Draft\n---\n")
        write(self.root, doc_path("SL-PLAN-001"), "tampered\n")
        write(self.root, doc_path("SL-REG-002"), "{broken")
        errors = self.run_gate()[0]
        self.assertEqual(rules(errors), {"R-V1-FILE"})
        self.assertEqual(len(errors), 5)

    def test_owner_missing_wo_list_and_disagreement(self):
        self.mutate(lambda d: d["documents"][0].update(authoring_wo="WO-099"))
        self.assertTrue(any("WO-099" in e and "R-V1-OWNER" in e for e in self.run_gate()[0]))
        self.setUp()
        self.mutate(lambda d: d["documents"][1].update(authoring_wo=["WO-001", "WO-002"]))
        self.assertIn("R-V1-OWNER", rules(self.run_gate()[0]))
        self.setUp()
        self.mutate(lambda d: d["documents"][2].update(authoring_wo=None))
        self.assertIn("R-V1-OWNER", rules(self.run_gate()[0]))

    def test_genesis_wo_is_exempt_but_must_agree_with_register(self):
        self.mutate(lambda d: d["documents"][0].update(authoring_wo="WO-000"))
        errors = self.run_gate()[0]
        self.assertEqual(len(errors), 1)
        self.assertIn("differs from", errors[0])

    def test_episodes(self):
        self.mutate(lambda d: d["episodes"].pop(4))
        errors = self.run_gate()[0]
        self.assertTrue(any("SL-EP-005 has no primary document" in e for e in errors))
        self.setUp()
        self.mutate(lambda d: d["episodes"].append(copy.deepcopy(d["episodes"][0])))
        self.assertTrue(any("appears 2 times" in e for e in self.run_gate()[0]))
        self.setUp()
        self.mutate(lambda d: d["episodes"][0].update(document="SL-GOV-099", wo="WO-098"))
        errors = self.run_gate()[0]
        self.assertEqual(rules(errors), {"R-V1-EPISODES"})
        self.assertEqual(len(errors), 2)
        self.setUp()
        self.mutate(
            lambda d: d["episodes"].append({"episode": "SL-EP-021", "document": gate.EXPECTED_IDS[0], "wo": WO})
        )
        self.assertTrue(any("outside 001-020" in e for e in self.run_gate()[0]))

    def test_contract_unknown_recipe_and_prose(self):
        build(
            self.root,
            '{"c": [{"id": "C1", "enforced_by": "just nothere"}, {"id": "C2", "verified_by": "somebody checks"}]}',
        )
        errors = self.run_gate()[0]
        self.assertEqual(rules(errors), {"R-V1-CONTRACT"})
        self.assertEqual(len(errors), 2)

    def test_contract_stub_recipe_is_pending_not_failure(self):
        build(
            self.root,
            '{"c": [{"id": "C1", "enforced_by": "just stubbed"}, {"id": "C2", "enforcement": "prose", "verified_by": '
            '"pending"}]}',
        )
        errors, rep = self.run_gate()
        self.assertEqual(errors, [])
        self.assertEqual(len(rep["pending"]), 2)

    def test_main_exit_codes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.main(["x", "--root", self.root]), 0)
            os.remove(os.path.join(self.root, gate.REL))
            self.assertEqual(gate.main(["x", "--root", self.root]), 1)


if __name__ == "__main__":
    unittest.main()
