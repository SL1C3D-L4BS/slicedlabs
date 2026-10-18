"""Unit tests of tools/fabric/slgen.py (WO-045). FAB-INV-GEN-01 (ARCH1-R68): the committed generated files equal the
slgen output, slgen is deterministic, and a stale, missing or extra generated file is a named failure."""

import contextlib
import io
import json
import os
import random
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schemalib  # noqa: E402
import slgen  # noqa: E402

ROOT = slgen.ROOT
FIXTURE_MODULES = os.path.join(ROOT, *schemalib.FIXTURE_MODULES_REL.split("/"))
HEADER = "engine/fabric/gen/include/sl/fabric/gen/fabric.history.gen.hpp"


def copy_root(tmp):
    """The parts of the repository slgen reads, copied under tmp."""
    for rel in ("schemas/fabric", "docs/arch", "engine/fabric/gen", slgen.FIXTURES_REL):
        shutil.copytree(os.path.join(ROOT, *rel.split("/")), os.path.join(tmp, *rel.split("/")))
    return tmp


def module_dir(tmp, modules):
    """A directory holding the given module documents."""
    directory = os.path.join(tmp, "mods")
    os.makedirs(directory, exist_ok=True)
    for m in modules:
        with open(os.path.join(directory, m["module"] + ".sltype.json"), "w", encoding="utf-8") as fh:
            json.dump(m, fh, indent=2)
    return directory


def fixture_modules():
    out = []
    for name in schemalib.module_files(FIXTURE_MODULES):
        with open(os.path.join(FIXTURE_MODULES, name), encoding="utf-8") as fh:
            out.append(json.load(fh))
    return out


def struct_type(name, fields, **extra):
    t = {"name": name, "kind": "struct", "version": "1.0", "record_class": "none", "state_class": "document",
         "doc": "A type.", "fields": fields}
    t.update(extra)
    return t


def field(no, name, type_, presence="required", **extra):
    f = {"no": no, "name": name, "type": type_, "presence": presence, "doc": "A field."}
    f.update(extra)
    return f


def module(name, types, domain=None):
    return {"format": "sl-type-def/1", "module": name, "domain": domain or name, "owner": "05", "doc": "Test.",
            "types": types}


class StableTest(unittest.TestCase):
    def test_fab_inv_gen_01_stable(self):
        first, _ = slgen.generate_set(ROOT)
        second, _ = slgen.generate_set(ROOT)
        self.assertEqual(first, second)
        findings, (modules, types, files) = slgen.check(ROOT)
        self.assertEqual(findings, [])
        self.assertEqual((modules, len(first)), (7, files))
        self.assertGreaterEqual(types, 7)
        for rel, text in first.items():
            self.assertTrue(text.endswith("\n"), rel)
            if rel.endswith((".hpp", ".cpp")):
                self.assertTrue(all(len(line) <= 100 for line in text.split("\n")), rel)
                self.assertNotRegex(text, r"\bthrow\b|\btry\b|\bcatch\b|\.at\(|std::unordered_", rel)

    def test_fab_inv_gen_01_every_system_module_is_generated(self):
        files, model = slgen.generate_set(ROOT)
        names = {m["module"] for m in model.modules}
        self.assertEqual(names, {"fabric.history", "fabric.identity", "fabric.manifest", "fabric.provenance",
                                 "fabric.schema", "fabric.edge", "fabric.event"})
        for name in names:
            for rel in ("engine/fabric/gen/include/sl/fabric/gen/%s.gen.hpp", "engine/fabric/gen/src/%s.gen.cpp",
                        "engine/fabric/gen/meta/%s.meta.json", "engine/fabric/gen/text/%s.text.schema.json"):
                self.assertIn(rel % name, files)
        hashes = json.loads(files["%s/%s" % (slgen.FIXTURES_REL, slgen.LOCK_HASHES)])["hashes"]
        self.assertEqual(sorted(hashes), sorted(model.types))


class NamedFailureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = copy_root(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def path(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def test_fab_inv_gen_01_stale_header_named(self):
        with open(self.path(HEADER), "a", encoding="utf-8") as fh:
            fh.write("// hand edit\n")
        findings, _ = slgen.check(self.root)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn(HEADER, findings[0])
        self.assertIn("R-GEN-STALE", findings[0])

    def test_fab_inv_gen_01_missing_and_extra_named(self):
        meta = "engine/fabric/gen/meta/fabric.edge.meta.json"
        os.remove(self.path(meta))
        extra = "engine/fabric/gen/src/handwritten.cpp"
        with open(self.path(extra), "w", encoding="utf-8") as fh:
            fh.write("int x;\n")
        text = "\n".join(slgen.check(self.root)[0])
        self.assertIn("%s: R-GEN-MISSING" % meta, text)
        self.assertIn("%s: R-GEN-EXTRA" % extra, text)
        slgen.generate(self.root)
        self.assertEqual(slgen.check(self.root)[0], [])
        self.assertFalse(os.path.exists(self.path(extra)))

    def test_fab_inv_gen_01_schema_edit_makes_output_stale(self):
        path = self.path("schemas/fabric/fabric.edge.sltype.json")
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        doc["types"][1]["fields"][0]["doc"] = "Edited."
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2)
        text = "\n".join(slgen.check(self.root)[0])
        for rel in ("engine/fabric/gen/src/fabric.edge.gen.cpp", "engine/fabric/gen/meta/fabric.edge.meta.json",
                    "tests/fixtures/fabric/types/lock_hashes.json"):
            self.assertIn("%s: R-GEN-STALE" % rel, text)

    def test_fab_inv_gen_01_fixtures_required(self):
        os.remove(self.path(slgen.FIXTURES_REL + "/fabric.edge/edge.json"))
        bad = self.path(slgen.FIXTURES_REL + "/fabric.schema/state_class.json")
        with open(bad, "w", encoding="utf-8") as fh:
            json.dump({"format": slgen.FIXTURE_FORMAT, "type": "fabric.schema.state_class",
                       "values": ["document", "liquid"]}, fh)
        stray = self.path(slgen.FIXTURES_REL + "/fabric.schema/nothing.json")
        with open(stray, "w", encoding="utf-8") as fh:
            fh.write("{}\n")
        text = "\n".join(slgen.check(self.root)[0])
        self.assertIn("fabric.edge/edge.json: R-GEN-FIXTURE: fabric.edge.edge needs fixtures", text)
        self.assertIn("fabric.schema/state_class.json: R-GEN-FIXTURE: value 1 is not canonical text", text)
        self.assertIn("fabric.schema/nothing.json: R-GEN-FIXTURE: names no generated type", text)

    def test_fab_inv_gen_01_cli(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(slgen.main(["slgen.py", "check", "--root", self.root]), 0)
        self.assertIn("fabric-gen-check: ok", out.getvalue())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(slgen.main(["slgen.py"]), 2)


class FixtureModulesTest(unittest.TestCase):
    """The generator against the WO-040 fixture modules: every type kind, refs, defaults, a reserved number."""

    def setUp(self):
        self.files, self.model = slgen.generate_set(ROOT, FIXTURE_MODULES, schemalib.FIXTURE_MODULES_REL,
                                                    require_docs=False)

    def source(self, module):
        return self.files["engine/fabric/gen/src/%s.gen.cpp" % module]

    def header(self, module):
        return self.files["engine/fabric/gen/include/sl/fabric/gen/%s.gen.hpp" % module]

    def test_fab_inv_gen_01_fixture_modules_deterministic(self):
        again, _ = slgen.generate_set(ROOT, FIXTURE_MODULES, schemalib.FIXTURE_MODULES_REL, require_docs=False)
        self.assertEqual(self.files, again)

    def test_fab_inv_gen_01_lock_hashes_equal_wo040_vectors(self):
        with open(os.path.join(ROOT, *schemalib.VECTORS_REL.split("/")), encoding="utf-8") as fh:
            vectors = {v["type"]: v["hash"] for v in json.load(fh)["vectors"]}
        hashes = json.loads(self.files["%s/%s" % (slgen.FIXTURES_REL, slgen.LOCK_HASHES)])["hashes"]
        self.assertEqual(hashes, vectors)

    def test_fab_inv_gen_01_every_kind_is_spelled(self):
        h = self.header("content.shapes")
        for spelling in ("enum class color : std::uint32_t {", "struct point {",
                         "using points = std::vector<content_shapes::point>;",
                         "using tags = std::map<std::string, std::string>;", "using meters = double;",
                         "std::variant<double, content_shapes::points> alt;",
                         "using layers = std::vector<gen::oset_item<content_shapes::shape>>;",
                         "std::optional<sim_physics::body> body;", "std::optional<fabric::record_id> parent;",
                         '#include "sl/fabric/gen/sim.physics.gen.hpp"'):
            self.assertIn(spelling, h)
        s = self.source("content.shapes")
        for spelling in ("t.fields.back().default_value = detail::f32_constant(1.5F);",
                         "t.fields.back().default_value = fabric::value::uint(3);",
                         "t.fields.back().default_value = fabric::value::boolean(true);",
                         "t.fields.back().since_minor = 1;", "t.reserved = {4};",
                         "t.fields.back().ref = detail::make_ref(fabric::ref_strength::weak);",
                         'detail::struct_reader r(in, "content.shapes.scene");',
                         "w.put_optional(8, in.body, sim_physics::body_to_value);",
                         "r.get(1, out.x, detail::dec_f64);", "t.record_class = fabric::id_class::entity;"):
            self.assertIn(spelling, s)
        p = self.source("sim.physics")
        self.assertIn("t.fields.back().default_value = fabric::value::integer(-5);", p)
        self.assertIn("t.fields.back().deprecated_in = 0;", p)
        self.assertIn("  static_ = 0,", self.header("sim.physics"))
        self.assertIn("gen::external_ref", self.header("runtime.capture"))

    def test_fab_inv_gen_01_state_class_only_on_records(self):
        s = self.source("content.shapes")
        self.assertEqual(s.count("t.state_class = "), 1)  # scene is the only record type of the module
        self.assertIn("out.back().declared_state = fabric::state_class::document;", s)


class RefusalTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def refused(self, modules, rule, require_docs=True):
        directory = module_dir(self.tmp, modules)
        with self.assertRaises(slgen.GenError) as ctx:
            slgen.generate_set(ROOT, directory, "mods", require_docs=require_docs)
        text = "\n".join(ctx.exception.findings)
        self.assertIn(rule, text)
        return text

    def test_fab_inv_gen_01_list_of_identity_bearing_struct(self):
        pair = struct_type("content.t.pair", [field(1, "who", "id")])
        lst = {"name": "content.t.pairs", "kind": "list", "version": "1.0", "record_class": "none",
               "state_class": "document", "doc": "Pairs.", "element": "content.t.pair"}
        self.assertIn("content.t.pairs", self.refused([module("content.t", [pair, lst])], "R-GEN-LIST-ID"))

    def test_fab_inv_gen_01_enumerator_since_minor(self):
        enum = {"name": "content.t.e", "kind": "enum", "version": "1.1", "record_class": "none",
                "state_class": "document", "doc": "E.",
                "enumerators": [{"no": 0, "name": "a", "doc": "A."}, {"no": 1, "name": "b", "since_minor": 1,
                                                                     "doc": "B."}]}
        self.refused([module("content.t", [enum])], "R-GEN-UNSUPPORTED")

    def test_fab_inv_gen_01_recursion_by_value(self):
        a = struct_type("content.t.a", [field(1, "b", "content.t.b", "optional")])
        b = struct_type("content.t.b", [field(1, "a", "content.t.a", "optional")])
        self.assertIn("reaches itself by value", self.refused([module("content.t", [a, b])], "R-GEN-UNSUPPORTED"))

    def test_fab_inv_gen_01_docs_required(self):
        t = struct_type("content.t.a", [field(1, "x", "u8")])
        del t["fields"][0]["doc"]
        self.assertIn("field x", self.refused([module("content.t", [t])], "R-GEN-DOC"))
        directory = module_dir(self.tmp, [module("content.t", [t])])
        files, _ = slgen.generate_set(ROOT, directory, "mods", require_docs=False)
        self.assertIn("engine/fabric/gen/src/content.t.gen.cpp", files)

    def test_fab_inv_gen_01_long_name(self):
        t = struct_type("content.t.a", [field(1, "x", "content.t." + "b" * 70)])
        t2 = struct_type("content.t." + "b" * 70, [field(1, "y", "u8")])
        self.refused([module("content.t", [t2, t])], "R-GEN-LONG")

    def test_fab_inv_gen_01_invalid_input(self):
        t = struct_type("content.t.a", [field(1, "x", "content.t.missing")])
        self.refused([module("content.t", [t])], "R-SCH-WELLFORMED")

    def test_fab_inv_gen_01_keywords_escaped(self):
        t = struct_type("content.t.class", [field(1, "new", "u8"), field(2, "in", "u8"), field(3, "class", "u8")])
        directory = module_dir(self.tmp, [module("content.t", [t])])
        files, _ = slgen.generate_set(ROOT, directory, "mods")
        h = files["engine/fabric/gen/include/sl/fabric/gen/content.t.gen.hpp"]
        self.assertIn("struct class_ {", h)
        self.assertIn("std::uint8_t new_{};", h)
        self.assertIn("std::uint8_t in_{};", h)
        self.assertIn("std::uint8_t class__{};", h)


class HelperTest(unittest.TestCase):
    def test_fab_inv_gen_01_range_regex(self):
        rnd = random.Random(45)
        for _ in range(60):
            lo = rnd.choice([0, 1, 9, 10, 99, 100, rnd.randrange(1 << 64)])
            hi = rnd.choice([lo, lo + rnd.randrange(1000), (1 << 64) - 1, lo * 10 + 7])
            pattern = re.compile("^(?:%s)$" % slgen.range_regex(lo, hi))
            probes = {lo, hi, max(lo - 1, 0), hi + 1, (lo + hi) // 2, 0, 10 ** len(str(hi))}
            probes |= {rnd.randrange(lo, hi + 1) for _ in range(20)}
            for n in probes:
                self.assertEqual(bool(pattern.match(str(n))), lo <= n <= hi, (lo, hi, n))

    def test_fab_inv_gen_01_string_split(self):
        lines = slgen.assign_string("  ", "t.doc", "word " * 60)
        self.assertGreater(len(lines), 2)
        self.assertTrue(all(len(line) <= 100 for line in lines))
        pad = len('  t.doc = ')
        self.assertTrue(all(line.startswith(" " * pad + '"') for line in lines[1:]))
        pieces = [re.search(r'"(.*)"', line).group(1) for line in lines]
        self.assertEqual("".join(pieces), "word " * 60)
        self.assertEqual(slgen.assign_string("  ", "t.doc", 'a "q" \\'), ['  t.doc = "a \\"q\\" \\\\";'])


if __name__ == "__main__":
    unittest.main()
