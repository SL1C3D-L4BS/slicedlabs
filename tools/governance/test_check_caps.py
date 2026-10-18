"""Unit tests for capability-check (cmd_check_caps)."""

import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_caps as caps  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402


def read(rel):
    with open(os.path.join(slgov.ROOT, *rel.split("/")), encoding="utf-8") as fh:
        return fh.read()


PLAN = read(caps.PLAN_REL)
ONTOLOGY = read(caps.ONTOLOGY_REL)
PARITY = read(caps.PARITY_REL)
REG = slib.load_json(os.path.join(slgov.ROOT, *caps.REGISTER_REL.split("/")))
SUBS = caps.plan_subsystems(PLAN)
ROWS = caps.plan_parity(PLAN)
DOC_IDS = {d["id"] for d in slib.load_json(os.path.join(slgov.ROOT, "registry", "SL-REG-001-documents.json"))["documents"]}
EP_IDS = {e["id"] for e in slib.load_json(os.path.join(slgov.ROOT, "registry", "SL-REG-002-episodes.json"))["episodes"]}


def mutated(fn):
    reg = copy.deepcopy(REG)
    fn(reg["capabilities"])
    return reg


class PlanParseTests(unittest.TestCase):
    def test_twelve_subsystems_and_parity_rows_counted_from_plan(self):
        self.assertEqual([s["n"] for s in SUBS], caps.SUBSYSTEMS)
        self.assertEqual(len(ROWS), PLAN.split("### **2.7")[1].split("### **2.8")[0].count("\n| ") - 2)
        self.assertEqual(ROWS[0]["unreal"], "Nanite virtualized geometry")
        self.assertEqual(ROWS[4]["volumes"], ["09", "28"])

    def test_register_has_one_capability_per_subsystem_and_per_row(self):
        c = REG["capabilities"]
        self.assertEqual(sum(1 for x in c if x["src"] == caps.SRC_SUBSYS), 12)
        self.assertEqual(sum(1 for x in c if x["unreal_counterpart"] is not None), len(ROWS))
        self.assertEqual(len(c), 12 + len(ROWS))


class RealDataTests(unittest.TestCase):
    def test_real_data_passes(self):
        self.assertEqual(caps.check_all(slgov.ROOT), [])


class RegisterTests(unittest.TestCase):
    def test_bad_boundary_and_extra_key(self):
        reg = mutated(lambda c: c[0].update(boundary="kernel"))
        self.assertTrue(any("R-CAP-SHAPE" in e and "boundary" in e for e in caps.shape_errors(reg)))
        reg = mutated(lambda c: c[0].update(owner="x"))
        self.assertTrue(any("unexpected ['owner']" in e for e in caps.shape_errors(reg)))

    def test_duplicate_id_and_id_subsystem_mismatch(self):
        reg = mutated(lambda c: c[1].update(id=c[0]["id"], subsystem=c[0]["subsystem"]))
        self.assertTrue(any("duplicate id" in e for e in caps.shape_errors(reg)))
        reg = mutated(lambda c: c[0].update(subsystem="02"))
        self.assertTrue(any("differs from subsystem" in e for e in caps.shape_errors(reg)))

    def test_subsystem_out_of_range(self):
        reg = mutated(lambda c: c[0].update(id="CAP-13-X", subsystem="13"))
        self.assertEqual(caps.shape_errors(reg), [])
        self.assertTrue(any("R-CAP-SUBSYS" in e and "not 01-12" in e for e in caps.ref_errors(reg, DOC_IDS, EP_IDS)))

    def test_unknown_interface_doc_and_episode(self):
        reg = mutated(lambda c: c[0].update(interface_doc="SL-ARCH-099", introduced_in="SL-EP-999"))
        errors = caps.ref_errors(reg, DOC_IDS, EP_IDS)
        self.assertTrue(any("R-CAP-DOC" in e for e in errors))
        self.assertTrue(any("R-CAP-EP" in e for e in errors))
        reg = mutated(lambda c: c[0].update(interface_doc="SL-GOV-001"))
        self.assertTrue(any("R-CAP-DOC" in e for e in caps.ref_errors(reg, DOC_IDS, EP_IDS)))

    def test_missing_top_level_key(self):
        self.assertTrue(caps.shape_errors({"schema": caps.SCHEMA_ID}))
        self.assertTrue(caps.shape_errors({"schema": "x", "capabilities": []}))


class CoverageTests(unittest.TestCase):
    def test_removed_parity_capability_fails(self):
        reg = mutated(lambda c: c.__delitem__(-1))
        errors = caps.parity_errors(reg, ROWS, PARITY)
        self.assertTrue(any("R-CAP-PARITY" in e and "Lyra sample game" in e for e in errors))

    def test_wrong_src_row_fails(self):
        reg = mutated(lambda c: c[12].update(src="SL-PLAN-001 §2.7 row 9"))
        self.assertTrue(any("src is" in e for e in caps.parity_errors(reg, ROWS, PARITY)))

    def test_removed_subsystem_capability_fails(self):
        reg = mutated(lambda c: c.__delitem__(0))
        self.assertTrue(any("subsystem 01" in e for e in caps.subsystem_errors(reg, SUBS, ONTOLOGY)))

    def test_document_row_drift_fails(self):
        text = PARITY.replace("Page-cache cost shown per light in the profiler", "Cheaper")
        self.assertTrue(any("row 3.obligation" in e for e in caps.parity_errors(REG, ROWS, text)))
        text = ONTOLOGY.replace("Workspace layouts, command registry, selection", "Layouts")
        self.assertTrue(any("subsystem 01.owns" in e for e in caps.subsystem_errors(REG, SUBS, text)))

    def test_missing_block_fails(self):
        self.assertTrue(any("no sl-data block" in e for e in caps.parity_errors(REG, ROWS, "# none\n")))
        self.assertTrue(any("no sl-data block" in e for e in caps.ontology_errors("# none\n")))

    def test_boundary_classes_must_match_episode_006(self):
        text = ONTOLOGY.replace('{"class": "hosted", "plan_words": "hosted services"}', '{"class": "cloud", "plan_words": "x"}')
        self.assertTrue(any("R-CAP-ONTOLOGY" in e for e in caps.ontology_errors(text)))


class ClaimTests(unittest.TestCase):
    OBS = [r["obligation"] for r in ROWS]

    def test_unquoted_claim_fails_quoted_rule_passes(self):
        self.assertTrue(caps.claim_errors("Sliced Labs rivals Unreal here.\n", "x.md"))
        self.assertTrue(caps.claim_errors("It beats Unreal in lighting.\n", "x.md"))
        self.assertTrue(caps.claim_errors("Faster and better than Unreal.\n", "x.md"))
        self.assertEqual(caps.claim_errors('A claim such as "rivals Unreal" MUST NOT ship.\n', "x.md"), [])

    def test_plan_obligation_is_exempt(self):
        ob = "Imports beat the manual path; fidelity reported"
        self.assertEqual(caps.claim_errors("| 1 | x | " + ob + " |\n", "x.md", [ob]), [])
        self.assertTrue(caps.claim_errors("| 1 | x | " + ob + " |\n", "x.md", []))

    def test_claim_in_register_fails(self):
        reg = mutated(lambda c: c[0].update(src="rivals Unreal"))
        self.assertTrue(any("R-CAP-CLAIM" in e for e in caps.register_claim_errors(reg)))

    def test_real_documents_have_no_claim(self):
        for text, rel in ((ONTOLOGY, caps.ONTOLOGY_REL), (PARITY, caps.PARITY_REL)):
            self.assertEqual(caps.claim_errors(text, rel, self.OBS), [])


if __name__ == "__main__":
    unittest.main()
