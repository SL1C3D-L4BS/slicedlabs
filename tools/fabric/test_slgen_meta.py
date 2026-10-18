"""Unit tests of the inspector metadata slgen writes (WO-045). FAB-INV-GEN-04 (ARCH1-R67): the metadata lists every
type and field of every module with doc, domain, owner, state class and ref targets."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schemalib  # noqa: E402
import slgen  # noqa: E402

ROOT = slgen.ROOT
META_DIR = os.path.join(ROOT, "engine", "fabric", "gen", "meta")
FIXTURE_MODULES = os.path.join(ROOT, *schemalib.FIXTURE_MODULES_REL.split("/"))
FIELD_KEYS = {"no", "name", "cpp", "type", "presence", "doc", "domain", "owner", "state_class", "since_minor",
              "ref_strength", "ref_targets"}


def committed_meta():
    out = {}
    for name in sorted(os.listdir(META_DIR)):
        with open(os.path.join(META_DIR, name), encoding="utf-8") as fh:
            doc = json.load(fh)
        out[doc["module"]] = doc
    return out


def source_modules(directory):
    out = {}
    for name in schemalib.module_files(directory):
        with open(os.path.join(directory, name), encoding="utf-8") as fh:
            doc = json.load(fh)
        out[doc["module"]] = doc
    return out


class MetadataTest(unittest.TestCase):
    def setUp(self):
        self.arch, errors = schemalib.load_arch(ROOT)
        self.assertEqual(errors, [])

    def owner_of(self, domain):
        return self.arch["registry"][domain.split(".")[0]]["owner"]

    def check_module(self, meta, source, require_docs):
        self.assertEqual(meta["format"], slgen.META_FORMAT)
        self.assertEqual((meta["domain"], meta["owner"]), (source["domain"], source["owner"]))
        self.assertEqual([t["name"] for t in meta["types"]], [t["name"] for t in source["types"]])
        for mt, st in zip(meta["types"], source["types"]):
            subject = mt["name"]
            self.assertEqual(mt["lock_hash"], schemalib.lock_hash(st, source["domain"]), subject)
            self.assertEqual(mt["owner"], self.owner_of(mt["domain"]), subject)
            self.assertIn(mt["state_class"], schemalib.STATE_CLASSES, subject)
            self.assertEqual([f["no"] for f in mt["fields"]], sorted(f["no"] for f in st.get("fields", [])), subject)
            by_no = {f["no"]: f for f in st.get("fields", [])}
            for f in mt["fields"]:
                label = "%s field %s" % (subject, f["name"])
                self.assertEqual(set(f) - {"deprecated_in", "default"}, FIELD_KEYS, label)
                self.assertEqual(f["doc"], by_no[f["no"]].get("doc", ""), label)
                if require_docs:
                    self.assertTrue(f["doc"], label)
                self.assertTrue(f["domain"].startswith(f["domain"].split(".")[0] + "."), label)
                self.assertEqual(f["owner"], self.owner_of(f["domain"]), label)
                self.assertIn(f["state_class"], schemalib.STATE_CLASSES, label)
                if f["type"] == "ref":
                    self.assertEqual(f["ref_targets"], sorted(by_no[f["no"]]["ref"]["targets"]), label)
                    self.assertIn(f["ref_strength"], schemalib.STRENGTHS, label)
                else:
                    self.assertEqual((f["ref_strength"], f["ref_targets"]), (None, []), label)

    def test_fab_inv_gen_04_metadata(self):
        meta = committed_meta()
        sources = source_modules(os.path.join(ROOT, "schemas", "fabric"))
        self.assertEqual(sorted(meta), sorted(sources))
        for name, doc in meta.items():
            self.check_module(doc, sources[name], require_docs=True)
            for t in doc["types"]:
                self.assertTrue(t["doc"], t["name"])

    def test_fab_inv_gen_04_fixture_modules(self):
        files, _ = slgen.generate_set(ROOT, FIXTURE_MODULES, schemalib.FIXTURE_MODULES_REL, require_docs=False)
        sources = source_modules(FIXTURE_MODULES)
        metas = {}
        for name in sources:
            metas[name] = json.loads(files["engine/fabric/gen/meta/%s.meta.json" % name])
            self.check_module(metas[name], sources[name], require_docs=False)
        scene = next(t for t in metas["content.shapes"]["types"] if t["name"] == "content.shapes.scene")
        fields = {f["name"]: f for f in scene["fields"]}
        # The innermost typed value whose domain differs governs a field (ARCH1-R39): body is sim's.
        self.assertEqual((fields["body"]["domain"], fields["body"]["owner"]), ("sim.physics", "06"))
        self.assertEqual((fields["name"]["domain"], fields["name"]["owner"]), ("content.shapes", "05"))
        self.assertEqual(fields["parent"]["ref_targets"], ["content.shapes.scene"])
        self.assertEqual(fields["parent"]["ref_strength"], "weak")
        self.assertEqual(fields["color"]["default"], 1)
        self.assertEqual(scene["reserved"], [4])
        capture = metas["runtime.capture"]["types"][0]
        self.assertEqual(capture["state_class"], "observation")
        self.assertEqual({f["name"]: f["ref_targets"] for f in capture["fields"]}["scene"], ["content.shapes.scene"])

    def test_fab_inv_gen_04_metadata_is_current(self):
        files, _ = slgen.generate_set(ROOT)
        for name, doc in committed_meta().items():
            self.assertEqual(json.loads(files["engine/fabric/gen/meta/%s.meta.json" % name]), doc, name)


if __name__ == "__main__":
    unittest.main()
