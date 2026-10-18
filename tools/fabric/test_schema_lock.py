"""Unit tests of the schema lock (WO-040). FAB-INV-SCH-02: a released (type, version) is immutable."""

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema_check  # noqa: E402
import schemalib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(ROOT, "tests", "fixtures", "fabric", "schema")
SCENE = "content.shapes.scene"
EMPTY_LOCK = {"format": "sl-schema-lock/1", "released": {}}


def make_root(tmp, with_modules=True):
    """A repository root with the schema file, SL-ARCH-001, an empty lock and (optionally) the fixture modules."""
    fabric = os.path.join(tmp, "schemas", "fabric")
    os.makedirs(fabric)
    shutil.copy(os.path.join(ROOT, schemalib.SCHEMA_REL), fabric)
    shutil.copytree(os.path.join(ROOT, "docs", "arch"), os.path.join(tmp, "docs", "arch"))
    with open(os.path.join(fabric, "schema.lock.json"), "w", encoding="utf-8") as fh:
        json.dump(EMPTY_LOCK, fh)
    if with_modules:
        src = os.path.join(FIXTURES, "modules")
        for name in os.listdir(src):
            shutil.copy(os.path.join(src, name), fabric)
    return tmp


def edit_type(root, module, name, fn):
    path = os.path.join(root, "schemas", "fabric", module + ".sltype.json")
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    fn(next(t for t in doc["types"] if t["name"] == name))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(doc, indent=2) + "\n")


def field(t, no):
    return next(f for f in t["fields"] if f["no"] == no)


class LockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="schema-lock-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = make_root(self.tmp)

    def lines(self):
        return schema_check.check(self.root)[0]

    def locked(self):
        added, errors = schema_check.lock_new(self.root)
        self.assertEqual(errors, [])
        return added

    def test_fab_inv_sch_02_released_immutable(self):
        self.assertEqual(self.lines(), [])
        added = self.locked()
        self.assertEqual(added, 12)
        self.assertEqual(self.lines(), [])
        # editing the type of a released field changes its lock hash and fails R-SCH-RELEASED naming the type
        edit_type(self.root, "content.shapes", SCENE, lambda t: field(t, 1).update(type="bytes"))
        errors = self.lines()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("R-SCH-RELEASED", errors[0])
        self.assertIn(SCENE + " 1.1", errors[0])
        # a label or doc edit changes the object id as well, so it is refused the same way
        edit_type(self.root, "content.shapes", SCENE, lambda t: field(t, 1).update(type="string", doc="changed"))
        self.assertIn("R-SCH-RELEASED", self.lines()[0])
        # restoring the definition passes again
        edit_type(self.root, "content.shapes", SCENE, lambda t: field(t, 1).update(doc="Display label."))
        self.assertEqual(self.lines(), [])

    def test_a_new_version_needs_the_bump_of_its_class(self):
        self.locked()
        # identical (doc only) with a minor bump passes; the old version stays pinned
        edit_type(self.root, "content.shapes", SCENE, lambda t: (field(t, 1).update(doc="x"), t.update(version="1.2")))
        self.assertEqual(self.lines(), [])
        # additive needs minor, breaking needs major
        edit_type(self.root, "content.shapes", SCENE, lambda t: t["fields"].append(
            {"no": 20, "name": "z", "type": "bool", "presence": "required"}))
        errors = self.lines()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("R-SCH-BUMP", errors[0])
        self.assertIn("major", errors[0])
        edit_type(self.root, "content.shapes", SCENE, lambda t: t.update(version="2.0"))
        self.assertEqual(self.lines(), [])

    def test_version_may_not_fall_and_a_released_type_may_not_leave(self):
        self.locked()
        edit_type(self.root, "content.shapes", SCENE, lambda t: t.update(version="1.0"))
        self.assertIn("R-SCH-RELEASED", self.lines()[0])
        edit_type(self.root, "content.shapes", SCENE, lambda t: t.update(version="1.1"))
        path = os.path.join(self.root, "schemas", "fabric", "runtime.capture.sltype.json")
        os.remove(path)
        errors = self.lines()
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("R-SCH-RELEASED", errors[0])
        self.assertIn("runtime.capture.frame_capture", errors[0])

    def test_lock_never_edits_an_existing_entry_and_refuses_a_bad_state(self):
        self.locked()
        path = os.path.join(self.root, "schemas", "fabric", "schema.lock.json")
        with open(path, encoding="utf-8") as fh:
            before = fh.read()
        self.assertEqual(self.locked(), 0)
        with open(path, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), before)
        edit_type(self.root, "content.shapes", SCENE, lambda t: field(t, 1).update(type="bytes"))
        added, errors = schema_check.lock_new(self.root)
        self.assertEqual(added, 0)
        self.assertIn("R-SCH-RELEASED", errors[0])
        with open(path, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), before)

    def test_a_tampered_lock_is_refused(self):
        self.locked()
        path = os.path.join(self.root, "schemas", "fabric", "schema.lock.json")
        with open(path, encoding="utf-8") as fh:
            lock = json.load(fh)
        lock["released"][SCENE]["1.1"]["hash"] = "sha256:" + "0" * 64
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(lock, fh)
        errors = self.lines()
        self.assertTrue(any("R-SCH-LOCK" in e and SCENE in e for e in errors), errors)

    def test_hash_vectors_match_the_fixture_file(self):
        doc, errors = schemalib.fixture_vectors(ROOT)
        self.assertEqual(errors, [])
        with open(os.path.join(ROOT, schemalib.VECTORS_REL), encoding="utf-8") as fh:
            text = fh.read()
        self.assertEqual(text, schemalib.render_vectors(doc))
        self.assertEqual(len(doc["vectors"]), 12)
        for row in doc["vectors"]:
            self.assertRegex(row["hash"], r"^sha256:[0-9a-f]{64}$")

    def test_lock_hash_is_the_schema_object_id(self):
        import hashlib
        with open(os.path.join(FIXTURES, "modules", "content.shapes.sltype.json"), encoding="utf-8") as fh:
            module = json.load(fh)
        t = module["types"][1]
        payload = schemalib.type_def_bytes(t, module["domain"])
        pre = b"slf-obj/1\x00" + bytes([1, 0, 9, 0]) + bytes([len(payload)]) + payload if len(payload) < 128 else None
        if pre is not None:
            self.assertEqual(schemalib.lock_hash(t, module["domain"]), "sha256:" + hashlib.sha256(pre).hexdigest())
        # the source order of fields and the omitted type-level doc do not change the hash
        shuffled = json.loads(json.dumps(t))
        shuffled["fields"].reverse()
        shuffled["doc"] = "other"
        self.assertEqual(schemalib.lock_hash(shuffled, module["domain"]), schemalib.lock_hash(t, module["domain"]))
        changed = json.loads(json.dumps(t))
        changed["fields"][0]["doc"] = "x"
        self.assertNotEqual(schemalib.lock_hash(changed, module["domain"]), schemalib.lock_hash(t, module["domain"]))


if __name__ == "__main__":
    unittest.main()
