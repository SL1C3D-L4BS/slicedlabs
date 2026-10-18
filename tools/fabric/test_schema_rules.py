"""Unit tests of the static schema rules (WO-040). FAB-INV-SCH-03: reserved reuse and class changes are refused."""

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema_check  # noqa: E402
import schemalib  # noqa: E402
from test_schema_lock import SCENE, edit_type, field, make_root  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHAPES = "content.shapes"
BODY = "sim.physics.body"


class RuleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="schema-rules-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = make_root(self.tmp)

    def lines(self):
        return schema_check.check(self.root)[0]

    def locked(self):
        added, errors = schema_check.lock_new(self.root)
        self.assertEqual(errors, [])
        return added

    def expect(self, rule, needle=None, count=None):
        errors = self.lines()
        hits = [e for e in errors if rule in e and (needle is None or needle in e)]
        self.assertTrue(hits, "expected %s %s in %r" % (rule, needle, errors))
        if count is not None:
            self.assertEqual(len(hits), count, errors)
        return errors

    def module_path(self, name):
        return os.path.join(self.root, "schemas", "fabric", name + ".sltype.json")

    def read_module(self, name):
        with open(self.module_path(name), encoding="utf-8") as fh:
            return json.load(fh)

    def write_module(self, name, doc):
        with open(self.module_path(name), "w", encoding="utf-8") as fh:
            fh.write(json.dumps(doc, indent=2) + "\n")

    # ---- SCH-03: forbidden changes ---------------------------------------------------------------------------

    def test_fab_inv_sch_03_forbidden(self):
        self.locked()
        # a reserved number used again by a later version
        edit_type(self.root, SHAPES, SCENE, lambda t: (t.update(version="1.2"), t["fields"].append(
            {"no": 4, "name": "again", "type": "bool", "presence": "optional"})))
        self.expect("R-SCH-REUSE", SCENE)
        edit_type(self.root, SHAPES, SCENE, lambda t: (t["fields"].pop(), t.update(version="1.1")))
        self.assertEqual(self.lines(), [])
        # dropping a number from the reserved list
        edit_type(self.root, SHAPES, SCENE, lambda t: (t.update(version="1.2", reserved=[])))
        self.expect("R-SCH-REUSE", "reserved number 4")
        # a removed field whose number is not reserved
        edit_type(self.root, SHAPES, SCENE, lambda t: (t.update(version="2.0", reserved=[4]), t["fields"].remove(
            field(t, 11))))
        self.expect("R-SCH-RESERVE", "field 11")
        edit_type(self.root, SHAPES, SCENE, lambda t: (t.update(version="2.0", reserved=[4, 11])))
        self.assertEqual(self.lines(), [])

    def test_domain_state_class_and_record_class_changes_are_forbidden(self):
        self.locked()
        for key, value in (("domain", "content.shapes.sub"), ("state_class", "derived"), ("record_class", "asset")):
            with self.subTest(key=key):
                edit_type(self.root, SHAPES, SCENE, lambda t: t.update(version="2.0", **{key: value}))
                self.expect("R-SCH-FORBIDDEN", SCENE)
                edit_type(self.root, SHAPES, SCENE, lambda t: (t.update(version="1.1"), t.pop(key)))
                edit_type(self.root, SHAPES, SCENE, lambda t: t.update(
                    state_class="document", record_class="entity"))
                self.assertEqual(self.lines(), [])

    def test_a_reserved_number_that_is_also_a_field_is_refused_without_a_lock(self):
        edit_type(self.root, SHAPES, SCENE, lambda t: t["reserved"].append(3))
        self.expect("R-SCH-REUSE", "field 3")

    # ---- state classes and ownership -------------------------------------------------------------------------

    def test_transient_state_is_not_representable(self):
        edit_type(self.root, SHAPES, "content.shapes.point", lambda t: t.update(state_class="transient"))
        self.expect("R-SCH-TRANSIENT", "content.shapes.point")

    def test_runtime_admits_observations_only(self):
        edit_type(self.root, "runtime.capture", "runtime.capture.frame_capture",
                  lambda t: t.update(state_class="document"))
        self.expect("R-OWN-STATE", "runtime.capture.frame_capture")

    def test_two_owners_for_one_domain_name_both_claimants(self):
        doc = self.read_module("sim.physics")
        doc["module"], doc["owner"] = "sim.physics_two", "07"
        for t in doc["types"]:
            t["name"] = t["name"].replace("sim.physics.", "sim.physics_two.")
            t["fields"] = [f for f in t.get("fields", []) if f["type"] != "ref"]
        doc["domain"] = "sim.physics"
        self.write_module("sim.physics_two", doc)
        errors = self.expect("R-OWN-DUP", "sim.physics")
        text = "\n".join(errors)
        self.assertIn("sim.physics.sltype.json owner 06", text)
        self.assertIn("sim.physics_two.sltype.json owner 07", text)

    def test_a_module_owner_must_match_the_domain_registry(self):
        doc = self.read_module("sim.physics")
        doc["owner"] = "05"
        self.write_module("sim.physics", doc)
        errors = self.expect("R-OWN-DUP", "owner 05")
        self.assertIn("owner 06", "\n".join(errors))

    def test_orphan_domains(self):
        doc = self.read_module("sim.physics")
        doc["domain"] = "widgets.physics"
        self.write_module("sim.physics", doc)
        self.expect("R-OWN-ORPHAN", "widgets.physics")
        doc["domain"] = "sim.physics"
        doc["types"][0]["domain"] = "sim.other"
        self.write_module("sim.physics", doc)
        self.expect("R-OWN-ORPHAN", "outside its module domain")
        self.assertEqual(schemalib.load_arch(ROOT)[0]["fabric_domains"][0], "fabric.history")

    def test_fabric_domains_are_the_six_of_the_registry(self):
        arch, _ = schemalib.load_arch(ROOT)
        doc = {"format": "sl-type-def/1", "module": "fabric.nothing", "domain": "fabric.nothing", "owner": "02",
               "types": [{"name": "fabric.nothing.x", "kind": "enum", "version": "1.0", "record_class": "none",
                          "state_class": "document", "enumerators": [{"no": 0, "name": "a"}]}]}
        self.write_module("fabric.nothing", doc)
        self.expect("R-OWN-ORPHAN", "fabric.nothing")
        self.assertEqual(len(arch["fabric_domains"]), 6)

    # ---- format and well-formedness --------------------------------------------------------------------------

    def test_format_errors_name_the_pointer(self):
        doc = self.read_module("sim.physics")
        doc["owner"] = "13"
        doc["extra"] = 1
        self.write_module("sim.physics", doc)
        errors = self.expect("R-SCH-FORMAT", "/owner")
        self.assertTrue(any("/extra" in e for e in errors))

    def test_wellformedness(self):
        def break_it(fn, needle):
            self.fresh()
            edit_type(self.root, SHAPES, SCENE, fn)
            self.expect("R-SCH-WELLFORMED", needle)

        break_it(lambda t: t["fields"].append({"no": 1, "name": "dup", "type": "bool", "presence": "optional"}),
                 "field number 1 is used 2 times")
        break_it(lambda t: t["fields"].append({"no": 40, "name": "name", "type": "bool", "presence": "optional"}),
                 "field name name")
        break_it(lambda t: field(t, 1).update(type="content.shapes.missing"), "unknown type")
        break_it(lambda t: field(t, 5).pop("ref"), "`ref` is required exactly when")
        break_it(lambda t: field(t, 5)["ref"].update(targets=["content.shapes.point"]), "not a record type")
        break_it(lambda t: field(t, 10).update(default="three"), "not an integer of u32")
        break_it(lambda t: field(t, 1).update(default="x"), "only an optional field may declare a default")
        break_it(lambda t: field(t, 6).update(default=1), "a default is allowed for")
        break_it(lambda t: field(t, 3).update(default=9), "not an enumerator number")
        break_it(lambda t: field(t, 12).update(default=1), "default of bool")
        break_it(lambda t: field(t, 11).update(default=0.1), "not a finite f32")
        break_it(lambda t: t.update(record_class="nope"), "neither none nor an id class")
        break_it(lambda t: t.update(kind="enum"), "not allowed on kind enum")

    def fresh(self):
        shutil.rmtree(self.root)
        make_root(self.root)

    def test_recursion_needs_an_optional_list_map_or_oset(self):
        self.write_module("content.loop", {
            "format": "sl-type-def/1", "module": "content.loop", "domain": "content.loop", "owner": "05",
            "types": [
                {"name": "content.loop.a", "kind": "struct", "version": "1.0", "record_class": "none",
                 "state_class": "document",
                 "fields": [{"no": 1, "name": "b", "type": "content.loop.b", "presence": "required"}]},
                {"name": "content.loop.b", "kind": "variant", "version": "1.0", "record_class": "none",
                 "state_class": "document", "cases": [{"no": 1, "name": "a", "type": "content.loop.a"}]},
                {"name": "content.loop.ok", "kind": "struct", "version": "1.0", "record_class": "none",
                 "state_class": "document",
                 "fields": [{"no": 1, "name": "me", "type": "content.loop.ok", "presence": "optional"}]}]})
        errors = self.expect("R-SCH-WELLFORMED", "recurses", count=1)
        self.assertIn("content.loop.a -> content.loop.b -> content.loop.a", errors[0] + "".join(errors))

    def test_names_and_files(self):
        shutil.copy(self.module_path("sim.physics"), self.module_path("other"))
        self.expect("R-SCH-NAME", "file name must be sim.physics.sltype.json")
        self.expect("R-SCH-NAME", "also defined in")

    def test_zero_modules_is_a_pass(self):
        empty = os.path.join(self.tmp, "empty")
        os.makedirs(empty)
        make_root(empty, with_modules=False)
        errors, counts = schema_check.check(empty)
        self.assertEqual((errors, counts), ([], (0, 0, 0)))

    def test_the_repository_passes_and_the_fixture_set_is_valid(self):
        errors, _counts = schema_check.check(ROOT)
        self.assertEqual(errors, [])
        self.assertEqual(self.lines(), [])
        self.assertEqual(schema_check.check(self.root)[1], (3, 12, 0))


if __name__ == "__main__":
    unittest.main()
