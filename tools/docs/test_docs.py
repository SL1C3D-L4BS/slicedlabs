"""Unit tests for tools/docs (WO-031): link and id checks, anchors, rebasing, page build."""

import importlib.util
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mdlib  # noqa: E402


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


check = load("sl_docs_check", "check.py")
build = load("sl_docs_build", "build.py")

DOCS = '{"documents": [{"id": "SL-GOV-001"}, {"id": "SL-VOL-03-SPEC"}, {"id": "SL-ARCH-001"}]}'
EPS = '{"episodes": [{"id": "SL-EP-041"}]}'


class Tree(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.write("registry/SL-REG-001-documents.json", DOCS)
        self.write("registry/SL-REG-002-episodes.json", EPS)
        self.write("docs/adr/ADR-0001-x.md", "# ADR\n")

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, text):
        path = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def findings(self):
        return check.run(self.root)[1]


class CheckTests(Tree):
    def test_clean(self):
        self.write(
            "docs/a.md", "# A\n\nSee [b](b.md#section-two) and SL-GOV-001, SL-EP-041, ADR-0001, SL-VOL-03-SPEC.\n"
        )
        self.write("docs/b.md", "# B\n\n## Section Two\n")
        self.assertEqual(self.findings(), [])

    def test_missing_file_and_anchor(self):
        self.write("docs/a.md", "[x](nope.md) [y](b.md#missing) [z](#top)\n")
        self.write("docs/b.md", "# B\n")
        out = self.findings()
        self.assertEqual(len(out), 3, out)
        self.assertTrue(all("R-DOCS-LINK" in e for e in out))

    def test_unknown_ids(self):
        self.write("docs/a.md", "SL-GOV-099 SL-EP-999 ADR-0042 SL-VOL-09-SPEC SL-OPS-001\n")
        out = self.findings()
        self.assertEqual(len(out), 5, out)
        self.assertTrue(all("R-DOCS-ID" in e for e in out))

    def test_fences_code_spans_external_and_skips(self):
        self.write(
            "docs/a.md",
            "```\n[x](nope.md) SL-GOV-099\n```\n`[y](nope.md)` [w](https://example.org/x) [m](mailto:a@b.c)\n",
        )
        self.write("docs/plan/SL-PLAN-001-master-plan.md", "[x](nope.md) SL-GOV-099\n")
        self.write("docs/templates/T.md", "SL-GOV-000 ADR-0000\n")
        self.assertEqual(self.findings(), [])

    def test_build_docs_and_readme_checked_but_not_ids(self):
        self.write("README.md", "[x](docs/missing.md)\n")
        self.write("build/docs/index.md", "[x](../../docs/a.md) SL-GOV-099\n")
        self.write("docs/a.md", "# A\n")
        out = self.findings()
        self.assertEqual(len(out), 1, out)
        self.assertIn("README.md:1: R-DOCS-LINK", out[0])

    def test_allow_list(self):
        self.write("docs/a.md", "[x](nope.md)\n")
        self.write("tools/docs/allow-links.txt", "docs/a.md|nope.md|owned by another work order\n")
        self.assertEqual(self.findings(), [])
        self.write("docs/a.md", "# A\n")
        self.assertIn("stale allow entry", self.findings()[0])
        self.write("tools/docs/allow-links.txt", "docs/a.md|nope.md\n")
        self.assertIn("allow entry needs", self.findings()[0])


class MdlibTests(unittest.TestCase):
    def test_anchors(self):
        text = "# Title `x`\n## A. B (c)\n## Dup\n## Dup\n```\n# not a heading\n```\n"
        self.assertEqual(mdlib.anchors(text), {"title-x", "a-b-c", "dup", "dup-1"})

    def test_rebase(self):
        src = "[a](docs/x.md#h) [b](#top) [c](https://e.org) `[d](keep.md)`\n```\n[e](keep.md)\n```\n[f](../up.md)"
        out = mdlib.rebase(src, "", "build/docs")
        self.assertIn("[a](../../docs/x.md#h)", out)
        self.assertIn("[b](#top) [c](https://e.org) `[d](keep.md)`", out)
        self.assertIn("[e](keep.md)", out)
        self.assertIn("[f](../../../up.md)", out)


class BuildTests(Tree):
    def make(self):
        self.write(
            "registry/SL-REG-001-documents.json",
            '{"documents": [{"id": "SL-GOV-001", "kind": "GOV", "status": "Planned"}]}',
        )
        self.write("registry/SL-REG-002-episodes.json", '{"episodes": [{"id": "SL-EP-041", "status": "Planned"}]}')
        self.write("README.md", "# R\n\n[a](docs/gov/G.md)\n")
        self.write("docs/gov/G.md", "# G\n")
        self.write(
            "docs/gov/SL-GOV-004-adr-log.md",
            "# L\n\n| ADR | Title |\n| :-- | :-- |\n| [ADR-0001](../adr/ADR-0001-x.md) | X |\n\nafter\n",
        )
        self.write("docs/adr/README.md", "# adr\n")
        for d in ("plan", "vol", "episodes", "build"):
            self.write("docs/%s/README.md" % d, "# %s\n" % d)
        self.write("docs/arch/README.md", "# arch\n")
        self.write("docs/ops/README.md", "# ops\n")
        for rel in (
            "README.md",
            "volumes.md",
            "volume-graph.mmd",
            "episodes.csv",
            "kohi-map.md",
            "SL-REG-005-dependencies.md",
        ):
            self.write("registry/views/" + rel, "x\n")
        self.write("registry/README.md", "# reg\n")
        self.write("docs/gov/SL-GOV-008-dependency-policy.md", "# dp\n")
        self.write("docs/adr/ADR-0002-dependency-policy.md", "# adr2\n")
        self.write("engine/core/include/sl/core/v.hpp", "// h\n")
        self.write(
            "deps/deps.lock.json",
            '{"policy": {"licenses_allowed": ["MIT"], "network": "never"}, "packages": {"a": {"version": "1", '
            '"license": "GPL-3.0", "role": "r", "tier": "A", "used_by": ["x"], "providers": {"vendored": '
            '{"verified_upstream": false}}}}}',
        )

    def test_stub_pages_are_link_clean_and_deterministic(self):
        self.make()
        out = os.path.join(self.root, "build", "docs")
        names = build.build(self.root, out, "stub")
        self.assertIn("index.md", names)
        first = {n: mdlib.read(os.path.join(out, n)) for n in names}
        build.build(self.root, out, "stub")
        self.assertEqual(first, {n: mdlib.read(os.path.join(out, n)) for n in names})
        self.assertIn("Stub page", first["api.md"])
        self.assertIn("GPL-3.0", first["dependencies.md"])
        self.assertIn("not verified against upstream", first["dependencies.md"])
        self.assertEqual(self.findings(), [])

    def test_doxygen_flag_needs_output(self):
        self.make()
        out = os.path.join(self.root, "build", "docs")
        with self.assertRaises(build.DocsBuildError):
            build.build(self.root, out, "doxygen")
        self.write("build/docs/api/html/index.html", "<html>")
        build.build(self.root, out, "doxygen")
        self.assertIn("api/html/index.html", mdlib.read(os.path.join(out, "api.md")))
        self.assertEqual(self.findings(), [])


if __name__ == "__main__":
    unittest.main()
