"""Unit tests for cmd_gen_docs.py, cmd_check_docs_registry.py and cmd_vol01_coverage.py (WO-005)."""

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_docs_registry  # noqa: E402
import cmd_gen_docs  # noqa: E402
import cmd_vol01_coverage  # noqa: E402
import slgov  # noqa: E402

REAL = slgov.ROOT


def doc_text(doc_id="SL-GOV-002", status="Draft", version="0.1.0"):
    return "---\nid: %s\nkind: GOV\ntitle: \"T\"\nstatus: %s\nversion: %s\nowner: founder\n---\n# T\n" % (
        doc_id,
        status,
        version,
    )


class TreeCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        for rel in (
            "docs/plan/SL-PLAN-001-master-plan.md",
            "registry/vol01-documents.json",
            "schemas/sl-reg-001.schema.json",
        ):
            self.put_copy(rel)
        os.makedirs(os.path.join(self.root, "work-orders"))
        for n in range(1, 24):
            self.write("work-orders/WO-%03d.md" % n, "x\n")
        self.regen()

    def tearDown(self):
        self._tmp.cleanup()

    def put_copy(self, rel):
        dst = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(REAL, *rel.split("/")), dst)

    def write(self, rel, text):
        path = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def regen(self):
        self.write(cmd_gen_docs.REGISTER_REL, cmd_gen_docs.render(cmd_gen_docs.build(self.root)))

    def doc(self, doc_id):
        return next(d for d in cmd_gen_docs.build(self.root)["documents"] if d["id"] == doc_id)

    def errors(self):
        return cmd_check_docs_registry.docs_errors(self.root)


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def read_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def rules(errors):
    return {e.split(": ")[1] for e in errors if ": R-" in e}


class GeneratorTests(TreeCase):
    def test_counts_per_kind(self):
        docs = cmd_gen_docs.build(self.root)["documents"]
        counts = {}
        for d in docs:
            counts[d["kind"]] = counts.get(d["kind"], 0) + 1
        self.assertEqual(counts, {"GOV": 10, "ARCH": 30, "PLAN": 5, "OPS": 10, "REG": 7, "VOL": 42})
        self.assertEqual(len(docs), 104)
        self.assertEqual(len({d["id"] for d in docs}), 104)

    def test_deterministic(self):
        a = cmd_gen_docs.render(cmd_gen_docs.build(self.root))
        b = cmd_gen_docs.render(cmd_gen_docs.build(self.root))
        self.assertEqual(a, b)
        self.assertEqual(json.loads(a)["schema"], "sl-reg-001/1")

    def test_constitution_title_and_alias(self):
        d = self.doc("SL-GOV-001")
        self.assertEqual(d["title"], "Platform Constitution and Engineering Doctrine")
        self.assertEqual(d["aliases"], ["Engine Constitution"])
        self.assertEqual(self.doc("SL-GOV-002")["aliases"], [])

    def test_vol01_paths(self):
        expect = {
            "SL-GOV-001": "docs/gov/SL-GOV-001-constitution.md",
            "SL-GOV-004": "docs/gov/SL-GOV-004-adr-log.md",
            "SL-GOV-010": "docs/gov/SL-GOV-010-parity-matrix.md",
            "SL-PLAN-001": "docs/plan/SL-PLAN-001-master-plan.md",
            "SL-PLAN-005": "docs/plan/SL-PLAN-005-kohi-alignment-map.md",
            "SL-REG-001": "registry/SL-REG-001-documents.json",
            "SL-REG-006": "registry/SL-REG-006-exceptions.json",
            "SL-VOL-01-SPEC": "docs/vol/SL-VOL-01-SPEC.md",
        }
        for doc_id, path in expect.items():
            self.assertEqual(self.doc(doc_id)["path"], path)

    def test_deferred_documents_and_episodes(self):
        arch = self.doc("SL-ARCH-001")  # placed by WO-035 (ARCH_SLUGS); later ARCH ids stay unplaced
        self.assertEqual(arch["path"], "docs/arch/SL-ARCH-001-canonical-project-model.md")
        self.assertIsNone(self.doc("SL-ARCH-003")["path"])
        self.assertEqual(arch["episodes"], ["SL-EP-%03d" % n for n in range(61, 81)])
        self.assertEqual(self.doc("SL-PLAN-001")["episodes"], [])
        self.assertEqual(self.doc("SL-GOV-003")["episodes"], ["SL-EP-005", "SL-EP-006", "SL-EP-017"])
        self.assertEqual(self.doc("SL-VOL-02-SPEC")["episodes"][0], "SL-EP-021")

    def test_arch_slugs_place_only_their_documents(self):
        self.assertEqual(
            self.doc("SL-ARCH-002")["path"], "docs/arch/SL-ARCH-002-transaction-undo-recovery.md"
        )  # not slugify(title): the slug is fixed in ARCH_SLUGS
        self.assertNotEqual(cmd_gen_docs.slugify(self.doc("SL-ARCH-002")["title"]), "transaction-undo-recovery")
        docs = cmd_gen_docs.build(self.root)["documents"]
        placed = sorted(d["id"] for d in docs if d["kind"] == "ARCH" and d["path"])
        self.assertEqual(placed, sorted(cmd_gen_docs.ARCH_SLUGS))
        self.assertTrue(all(d["path"] is None for d in docs if d["kind"] == "OPS"))

    def test_authoring_wo(self):
        self.assertEqual(self.doc("SL-REG-001")["authoring_wo"], "WO-005")
        self.assertEqual(self.doc("SL-GOV-004")["authoring_wo"], "WO-003")
        self.assertIsNone(self.doc("SL-ARCH-001")["authoring_wo"])

    def test_volume_written_in_parse(self):
        p = cmd_gen_docs.parse_volumes_written
        self.assertEqual(p("Vol 01, ongoing"), [1])
        self.assertEqual(p("Vol 11–14"), [11, 12, 13, 14])
        self.assertEqual(p("Vol 01, Vol 26"), [1, 26])
        self.assertEqual(p("Vol 07, 26"), [7, 26])
        self.assertEqual(p("Now, maintained"), [])

    def test_plan_without_section_is_an_error(self):
        with self.assertRaises(cmd_gen_docs.DocsError):
            cmd_gen_docs.parse_plan("# nothing\n")

    def test_real_register_is_current(self):
        with open(os.path.join(REAL, cmd_gen_docs.REGISTER_REL), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), cmd_gen_docs.render(cmd_gen_docs.build(REAL)))


class DeriveTests(TreeCase):
    def test_absent_files_are_draft(self):
        self.assertEqual(self.errors(), [])
        view, _ = cmd_gen_docs.derive_view(self.root, cmd_gen_docs.build(self.root), {})
        gov2 = next(d for d in view if d["id"] == "SL-GOV-002")
        self.assertEqual((gov2["status"], gov2["version"], gov2["sha256"]), ("Draft", None, None))

    def test_front_matter_status_version_sha(self):
        text = doc_text(status="InReview", version="0.2.0")
        self.write("docs/gov/SL-GOV-002-requirements-catalogue.md", text)
        view, problems = cmd_gen_docs.derive_view(self.root, cmd_gen_docs.build(self.root), {})
        d = next(x for x in view if x["id"] == "SL-GOV-002")
        self.assertEqual((d["status"], d["version"]), ("InReview", "0.2.0"))
        self.assertEqual(d["sha256"], hashlib.sha256(text.encode()).hexdigest())
        self.assertEqual(problems, [])
        self.assertEqual(self.errors(), [])

    def test_committed_register_stays_current_when_a_doc_lands(self):
        self.write("docs/gov/SL-GOV-002-requirements-catalogue.md", doc_text())
        self.assertNotIn("R-DOCS-DRIFT", rules(self.errors()))

    def test_bad_front_matter_status(self):
        self.write("docs/gov/SL-GOV-002-requirements-catalogue.md", doc_text(status="Done"))
        self.assertIn("R-DOCS-FRONT", rules(self.errors()))

    def test_front_matter_id_mismatch(self):
        self.write("docs/gov/SL-GOV-002-requirements-catalogue.md", doc_text(doc_id="SL-GOV-003"))
        self.assertIn("R-DOCS-FRONT", rules(self.errors()))


class ApprovalTests(TreeCase):
    PATH = "docs/gov/SL-GOV-002-requirements-catalogue.md"

    def sha(self):
        return hashlib.sha256(read_bytes(os.path.join(self.root, self.PATH))).hexdigest()

    def test_approved_needs_approval_entry(self):
        self.write(self.PATH, doc_text(status="Approved"))
        self.assertIn("R-DOCS-APPROVAL", rules(self.errors()))

    def test_approved_with_matching_sha(self):
        self.write(self.PATH, doc_text(status="Approved"))
        self.write(cmd_gen_docs.APPROVALS_REL, json.dumps({"SL-GOV-002": {"sha256": self.sha(), "version": "1.0.0"}}))
        self.assertEqual(self.errors(), [])
        view, _ = cmd_gen_docs.derive_view(
            self.root, cmd_gen_docs.build(self.root), cmd_gen_docs.load_approvals(self.root)[0]
        )
        d = next(x for x in view if x["id"] == "SL-GOV-002")
        self.assertEqual((d["status"], d["version"]), ("Approved", "1.0.0"))

    def test_approval_with_wrong_sha(self):
        self.write(self.PATH, doc_text(status="Approved"))
        self.write(cmd_gen_docs.APPROVALS_REL, json.dumps({"SL-GOV-002": {"sha256": "0" * 64}}))
        errs = self.errors()
        self.assertIn("R-DOCS-APPROVAL", rules(errs))
        self.assertTrue(any("does not match" in e for e in errs))

    def test_approval_for_unknown_or_absent_document(self):
        self.write(
            cmd_gen_docs.APPROVALS_REL,
            json.dumps({"SL-GOV-099": {"sha256": "0" * 64}, "SL-GOV-003": {"sha256": "0" * 64}}),
        )
        self.assertEqual(len([e for e in self.errors() if "R-DOCS-APPROVAL" in e]), 2)

    def test_approvals_must_be_an_object(self):
        self.write(cmd_gen_docs.APPROVALS_REL, "[]")
        self.assertIn("R-DOCS-INPUT", rules(self.errors()))


class RegistryTests(TreeCase):
    def test_clean_tree(self):
        self.assertEqual(self.errors(), [])

    def test_drift(self):
        path = os.path.join(self.root, *cmd_gen_docs.REGISTER_REL.split("/"))
        data = read_json(path)
        data["documents"][0]["title"] = "Edited by hand"
        self.write(cmd_gen_docs.REGISTER_REL, json.dumps(data, indent=2) + "\n")
        self.assertIn("R-DOCS-DRIFT", rules(self.errors()))

    def test_schema_violation(self):
        path = os.path.join(self.root, *cmd_gen_docs.REGISTER_REL.split("/"))
        data = read_json(path)
        data["documents"][0]["status"] = "Done"
        self.write(cmd_gen_docs.REGISTER_REL, json.dumps(data, indent=2) + "\n")
        self.assertIn("R-DOCS-SCHEMA", rules(self.errors()))

    def test_unregistered_files(self):
        self.write("docs/gov/SL-GOV-050-extra.md", doc_text("SL-GOV-050"))
        self.write("docs/gov/README.md", "readme\n")
        self.write("registry/SL-REG-009-extra.json", "{}")
        self.write("registry/SL-REG-001.approvals.json", "{}")
        errs = [e for e in self.errors() if "R-DOCS-UNREGISTERED" in e]
        self.assertEqual(
            sorted(e.split(":")[0] for e in errs), ["docs/gov/SL-GOV-050-extra.md", "registry/SL-REG-009-extra.json"]
        )

    def test_unregistered_arch_files(self):
        arch = doc_text("SL-ARCH-001").replace("kind: GOV", "kind: ARCH")
        self.write("docs/arch/SL-ARCH-001-canonical-project-model.md", arch)
        self.write("docs/arch/SL-ARCH-003-runtime.md", doc_text("SL-ARCH-003").replace("kind: GOV", "kind: ARCH"))
        self.write("docs/arch/README.md", "readme\n")
        errs = [e for e in self.errors() if "R-DOCS-UNREGISTERED" in e]
        self.assertEqual([e.split(":")[0] for e in errs], ["docs/arch/SL-ARCH-003-runtime.md"])

    def test_arch_front_matter_is_derived(self):
        arch = doc_text("SL-ARCH-002").replace("kind: GOV", "kind: ARCH")
        self.write("docs/arch/SL-ARCH-002-transaction-undo-recovery.md", arch)
        view, problems = cmd_gen_docs.derive_view(self.root, cmd_gen_docs.build(self.root), {})
        d = next(x for x in view if x["id"] == "SL-ARCH-002")
        self.assertEqual((d["status"], d["version"], problems), ("Draft", "0.1.0", []))
        self.write("docs/arch/SL-ARCH-002-transaction-undo-recovery.md", doc_text("SL-ARCH-009"))
        self.assertIn("R-DOCS-FRONT", rules(self.errors()))

    def test_missing_register(self):
        os.remove(os.path.join(self.root, *cmd_gen_docs.REGISTER_REL.split("/")))
        self.assertIn("R-DOCS-SCHEMA", rules(self.errors()))


class CoverageTests(TreeCase):
    def edit(self, fn):
        path = os.path.join(self.root, *cmd_vol01_coverage.REL.split("/"))
        data = read_json(path)
        fn(data)
        self.write(cmd_vol01_coverage.REL, json.dumps(data))

    def errors(self):
        return cmd_vol01_coverage.coverage_errors(self.root)

    def test_real_file_is_sound(self):
        self.assertEqual(self.errors(), [])
        self.assertEqual(cmd_vol01_coverage.coverage_errors(REAL), [])

    def test_missing_document(self):
        self.edit(lambda d: d["documents"].pop(0))
        self.assertIn("R-V01-DOCS", rules(self.errors()))

    def test_two_owners(self):
        self.edit(lambda d: d["documents"].append({"id": "SL-GOV-001", "authoring_wo": "WO-007"}))
        self.assertTrue(any("two owners" in e for e in self.errors()))

    def test_owner_list_or_null(self):
        self.edit(lambda d: d["documents"][0].update(authoring_wo=["WO-006", "WO-007"]))
        self.assertIn("R-V01-OWNER", rules(self.errors()))
        self.edit(lambda d: d["documents"][0].update(authoring_wo=None))
        self.assertIn("R-V01-OWNER", rules(self.errors()))

    def test_owner_file_missing(self):
        self.edit(lambda d: d["documents"][0].update(authoring_wo="WO-099"))
        self.assertTrue(any("does not exist" in e for e in self.errors()))

    def test_episode_without_document(self):
        self.edit(lambda d: d["episodes"].pop(4))
        self.assertTrue(any("SL-EP-005 has no primary document" in e for e in self.errors()))

    def test_episode_with_two_documents(self):
        self.edit(lambda d: d["episodes"].append({"episode": "SL-EP-001", "document": "SL-GOV-006", "wo": "WO-006"}))
        self.assertTrue(any("two primary documents" in e for e in self.errors()))

    def test_episode_outside_vol01_document_set(self):
        self.edit(lambda d: d["episodes"][0].update(document="SL-ARCH-001"))
        self.assertIn("R-V01-EPISODES", rules(self.errors()))

    def test_episode_map_matches_work_order(self):
        data = read_json(os.path.join(REAL, *cmd_vol01_coverage.REL.split("/")))
        got = {e["episode"]: (e["document"], e["wo"]) for e in data["episodes"]}
        self.assertEqual(got["SL-EP-008"], ("SL-GOV-007", "WO-009"))
        self.assertEqual(got["SL-EP-017"], ("SL-GOV-003", "WO-008"))
        self.assertEqual(got["SL-EP-020"], ("SL-GOV-001", "WO-023"))


if __name__ == "__main__":
    unittest.main()
