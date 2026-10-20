"""Unit tests for the governance helpers: FACTORY.md parser, recipes.json consistency, gate checks."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

JUSTFILE = os.path.join(slgov.ROOT, "justfile")
RECIPES_JSON = os.path.join(slgov.ROOT, "tools", "governance", "recipes.json")


def real_recipes():
    with open(JUSTFILE, encoding="utf-8") as fh:
        return slgov.parse_justfile(fh.read())


class FrontMatterTests(unittest.TestCase):
    def test_scalars_nested_map_and_flow_list(self):
        text = (
            "---\n"
            "# a comment\n"
            "id: WO-001\n"
            "risk: [ci, gpu]\n"
            'goal: "quoted # not a comment"\n'
            "gate:\n"
            "  fast: just fast   # trailing comment\n"
            "  full: just check\n"
            "governed: [registry/**, docs/gov/SL-GOV-001*]\n"
            "non_goals:\n"
            '  - "first"\n'
            "  - second\n"
            "---\n"
            "body\n"
        )
        meta = slgov.parse_front_matter(text)
        self.assertEqual(meta["id"], "WO-001")
        self.assertEqual(meta["risk"], ["ci", "gpu"])
        self.assertEqual(meta["goal"], "quoted # not a comment")
        self.assertEqual(meta["gate"], {"fast": "just fast", "full": "just check"})
        self.assertEqual(meta["governed"], ["registry/**", "docs/gov/SL-GOV-001*"])
        self.assertEqual(meta["non_goals"], ["first", "second"])

    def test_rejects_missing_or_open_front_matter(self):
        with self.assertRaises(slgov.FrontMatterError):
            slgov.parse_front_matter("no front matter\n")
        with self.assertRaises(slgov.FrontMatterError):
            slgov.parse_front_matter("---\nid: x\n")

    def test_rejects_duplicate_key_and_bad_line(self):
        with self.assertRaises(slgov.FrontMatterError):
            slgov.parse_front_matter("---\nid: a\nid: b\n---\n")
        with self.assertRaises(slgov.FrontMatterError):
            slgov.parse_front_matter("---\nnot a pair\n---\n")

    def test_real_factory_md(self):
        with open(os.path.join(slgov.ROOT, "FACTORY.md"), encoding="utf-8") as fh:
            meta = slgov.parse_front_matter(fh.read())
        self.assertEqual(meta["gate"]["fast"], "just fast")
        self.assertEqual(meta["gate"]["full"], "just check")
        self.assertIn("registry/**", meta["governed"])
        self.assertIn("work-orders/**", meta["governed"])


class JustfileTests(unittest.TestCase):
    TEXT = (
        'set shell := ["bash", "-euo", "pipefail", "-c"]\n'
        'version := "1"\n'
        "\n"
        "# comment\n"
        "default:\n"
        "    @just --list\n"
        "\n"
        "fast: a b\n"
        "    @echo hi\n"
        "a:\n"
        "    @just stub a\n"
        "b name='x':\n"
        "    @echo {{name}}\n"
    )

    def test_parse_names_deps_and_params(self):
        r = slgov.parse_justfile(self.TEXT)
        self.assertEqual(sorted(r), ["a", "b", "default", "fast"])
        self.assertEqual(r["fast"]["deps"], ["a", "b"])
        self.assertEqual(r["b"]["params"], ["name='x'"])
        self.assertEqual(slgov.stub_target(r["a"]), "a")
        self.assertIsNone(slgov.stub_target(r["default"]))

    def test_closure(self):
        r = slgov.parse_justfile(self.TEXT)
        self.assertEqual(slgov.closure(r, ["fast"]), {"fast", "a", "b"})

    def test_check_recipe_starts_with_fast(self):
        # WO-020 ratcheted `check`; `fast` (the stop-gate) must stay its first dependency.
        r = real_recipes()
        self.assertEqual(r["check"]["deps"][0], "fast")
        self.assertIn("ci-local", r["check"]["deps"])


class RecipesJsonTests(unittest.TestCase):
    def test_justfile_and_recipes_json_agree(self):
        manifest = slgov.load_recipes_json(RECIPES_JSON)
        self.assertEqual(slgov.consistency_errors(real_recipes(), manifest), [])

    def test_detects_missing_and_extra(self):
        recipes = slgov.parse_justfile("a:\n    @echo a\nb:\n    @echo b\n")
        manifest = {
            "a": {"owner": "WO-001", "status": "implemented"},
            "c": {"owner": "WO-001", "status": "implemented"},
        }
        errors = slgov.consistency_errors(recipes, manifest)
        self.assertTrue(any("'b' is in the justfile" in e for e in errors))
        self.assertTrue(any("'c' is in recipes.json" in e for e in errors))

    def test_detects_stub_status_mismatch(self):
        recipes = slgov.parse_justfile("a:\n    @just stub a\nb:\n    @echo b\n")
        manifest = {
            "a": {"owner": "WO-009", "status": "implemented"},
            "b": {"owner": "WO-009", "status": "stub"},
        }
        errors = slgov.consistency_errors(recipes, manifest)
        self.assertEqual(len(errors), 2)

    def test_owner_must_look_like_a_wo(self):
        recipes = slgov.parse_justfile("a:\n    @echo a\n")
        errors = slgov.consistency_errors(recipes, {"a": {"owner": "me", "status": "implemented"}})
        self.assertTrue(any("owner" in e for e in errors))

    def test_selftest_flags_landed_stub_in_closure(self):
        recipes = slgov.parse_justfile("fast: a\n    @echo f\na:\n    @just stub a\n")
        manifest = {
            "fast": {"owner": "WO-001", "status": "implemented"},
            "a": {"owner": "WO-001", "status": "stub"},
        }
        errors = slgov.selftest_errors(recipes, manifest, {"WO-001"}, roots=("fast",))
        self.assertTrue(any("has landed" in e for e in errors))
        self.assertEqual(slgov.selftest_errors(recipes, manifest, set(), roots=("fast",)), [])

    def test_real_gate_closure_has_no_wo001_stub(self):
        manifest = slgov.load_recipes_json(RECIPES_JSON)
        recipes = real_recipes()
        for name in slgov.closure(recipes, ["fast", "check"]):
            entry = manifest[name]
            self.assertFalse(entry["status"] == "stub" and entry["owner"] == "WO-001", name)

    def test_stub_recipes_are_not_in_fast(self):
        recipes = real_recipes()
        manifest = slgov.load_recipes_json(RECIPES_JSON)
        stubs = {n for n, e in manifest.items() if e["status"] == "stub"}
        self.assertEqual(slgov.closure(recipes, ["fast"]) & stubs, set())


class DispatchTests(unittest.TestCase):
    """Recipe status `dispatch` (WO-075): `bash scripts/runtime/<name>.sh {{args}}`, stub while the script is."""

    STUB = "#!/usr/bin/env bash\nexec python3 -I tools/governance/cmd_stub.py %s\n"
    TEXT = "fast: r\n    @echo f\nr *args:\n    @bash scripts/runtime/r.sh {{args}}\n"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        os.makedirs(os.path.join(self.root, "scripts", "runtime"))
        self.recipes = slgov.parse_justfile(self.TEXT)
        self.manifest = {
            "fast": {"owner": "WO-001", "status": "implemented"},
            "r": {"owner": "WO-076", "status": "dispatch"},
        }

    def tearDown(self):
        self._tmp.cleanup()

    def script(self, text):
        with open(os.path.join(self.root, "scripts", "runtime", "r.sh"), "w", encoding="utf-8") as fh:
            fh.write(text)

    def test_dispatch_body(self):
        self.assertTrue(slgov.is_dispatch_body("r", self.recipes["r"]))
        self.assertFalse(slgov.is_dispatch_body("s", self.recipes["r"]))
        other = slgov.parse_justfile("r args:\n    @bash scripts/runtime/r.sh {{args}}\n")
        self.assertFalse(slgov.is_dispatch_body("r", other["r"]))

    def test_stub_script_is_a_stub_and_a_real_script_is_not(self):
        self.assertFalse(slgov.is_dispatch_stub(self.root, "r"))
        self.script(self.STUB % "r")
        self.assertTrue(slgov.is_dispatch_stub(self.root, "r"))
        self.script(self.STUB % "other")
        self.assertFalse(slgov.is_dispatch_stub(self.root, "r"))
        self.script("#!/usr/bin/env bash\nset -euo pipefail\necho real\n")
        self.assertFalse(slgov.is_dispatch_stub(self.root, "r"))

    def test_consistency_accepts_dispatch_with_script(self):
        self.script(self.STUB % "r")
        self.assertEqual(slgov.consistency_errors(self.recipes, self.manifest, self.root), [])

    def test_consistency_flags_missing_script_and_wrong_body(self):
        errors = slgov.consistency_errors(self.recipes, self.manifest, self.root)
        self.assertEqual(errors, ["recipe 'r' is marked dispatch but scripts/runtime/r.sh is missing"])
        recipes = slgov.parse_justfile("fast:\n    @echo f\nr *args:\n    @bash scripts/other.sh {{args}}\n")
        errors = slgov.consistency_errors(recipes, self.manifest, self.root)
        self.assertTrue(any("marked dispatch but it does not take *args" in e for e in errors), errors)

    def test_selftest_treats_a_dispatch_stub_like_a_stub(self):
        self.script(self.STUB % "r")
        errors = slgov.selftest_errors(self.recipes, self.manifest, {"WO-076"}, roots=("fast",), root=self.root)
        self.assertEqual(errors, ["recipe 'r' is a stub in the gate closure but its owner WO-076 has landed"])
        self.assertEqual(slgov.selftest_errors(self.recipes, self.manifest, set(), ("fast",), self.root), [])
        self.script("#!/usr/bin/env bash\nset -euo pipefail\necho real\n")
        self.assertEqual(slgov.selftest_errors(self.recipes, self.manifest, {"WO-076"}, ("fast",), self.root), [])

    def test_real_dispatch_recipes(self):
        manifest = slgov.load_recipes_json(RECIPES_JSON)
        recipes = real_recipes()
        dispatch = sorted(n for n, e in manifest.items() if e["status"] == "dispatch")
        self.assertEqual(len(dispatch), 24)
        for name in dispatch:
            self.assertTrue(slgov.is_dispatch_body(name, recipes[name]), name)
        self.assertEqual(slgov.closure(recipes, ["fast", "check"]) & set(dispatch), set())

class LayoutTests(unittest.TestCase):
    def test_real_repo_layout(self):
        import json

        with open(os.path.join(slgov.ROOT, "tools", "governance", "layout.json"), encoding="utf-8") as fh:
            layout = json.load(fh)
        self.assertEqual(slgov.layout_errors(slgov.ROOT, layout, []), [])

    def test_missing_dir_readme_zero_byte_and_gitmodules(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "a"))
            os.makedirs(os.path.join(root, "b"))
            with open(os.path.join(root, "b", "README.md"), "w") as fh:
                fh.write("x\n")
            open(os.path.join(root, "empty.txt"), "w").close()
            layout = {"dirs": ["a", "b", "c"]}
            errors = slgov.layout_errors(root, layout, ["empty.txt", ".gitmodules", "gone.txt"])
        joined = "\n".join(errors)
        self.assertIn("missing README.md in: a", joined)
        self.assertIn("missing directory: c", joined)
        self.assertIn("empty.txt: tracked 0-byte file", joined)
        self.assertIn(".gitmodules", joined)
        self.assertNotIn("gone.txt", joined)
        self.assertNotIn("in: b", joined)


class GateTests(unittest.TestCase):
    def test_real_gates_pass(self):
        self.assertEqual(slgov.gate_errors(slgov.ROOT), [])

    def test_fetch_pattern(self):
        for bad in ("curl -s x", "wget x", "git fetch origin", "  git   fetch"):
            self.assertTrue(slgov.FORBIDDEN_FETCH.search(bad), bad)
        for ok in ("git ls-files", "curly braces", "git fetching"):
            self.assertFalse(slgov.FORBIDDEN_FETCH.search(ok), ok)

    def test_gate_errors_on_bad_root(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "FACTORY.md"), "w") as fh:
                fh.write("---\ngate:\n  fast: make fast\n  full: just nope\ngoverned: [a]\n---\n")
            with open(os.path.join(root, "justfile"), "w") as fh:
                fh.write("fast:\n    @echo fast\n")
            with open(os.path.join(root, ".gitignore"), "w") as fh:
                fh.write("build/\n")
            errors = slgov.gate_errors(root)
        joined = "\n".join(errors)
        self.assertIn("gate.fast must be `just <recipe>`", joined)
        self.assertIn("recipe 'nope'", joined)
        self.assertIn("/.bashrc", joined)
        self.assertIn("/.claude/*", joined)

