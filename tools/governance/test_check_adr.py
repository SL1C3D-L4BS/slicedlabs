"""Unit tests for adr-check (cmd_check_adr.py) and the ADR log generator (cmd_gen_adr_log.py)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_adr  # noqa: E402
import cmd_check_docs  # noqa: E402
import cmd_gen_adr_log  # noqa: E402
import slgov  # noqa: E402


def adr_text(adr_id="ADR-0001", status="Proposed", supersedes="[]", refs="[D-126]", extra=""):
    return (
        "---\n"
        "id: %s\n"
        'title: "A decision"\n'
        "status: %s\n"
        "date: 2026-10-17\n"
        "decision_refs: %s\n"
        "supersedes: %s\n"
        "---\n"
        "# %s: A decision\n\n"
        "## Context\n\nWhy (master plan section 1.5).\n\n"
        "## Decision\n\nIt MUST be so (master plan section 1.5).\n%s\n"
        "## Consequences\n\nSome.\n\n"
        "## Alternatives\n\nOthers.\n" % (adr_id, status, refs, supersedes, adr_id, extra)
    )


def rules(errors):
    return {e.split(": ")[1] for e in errors}


class TreeCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.addCleanup(self._tmp.cleanup)

    def adr(self, name, text):
        path = os.path.join(self.root, "docs", "adr", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def regenerate(self):
        path = os.path.join(self.root, *cmd_gen_adr_log.LOG_REL.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(cmd_gen_adr_log.render(self.root))

    def errors(self):
        self.regenerate()
        return cmd_check_adr.adr_errors(self.root)

    def good_tree(self):
        self.adr("ADR-0001-repo-layout.md", adr_text())


class AdrCheckTests(TreeCase):
    def test_minimal_good_tree_passes(self):
        self.good_tree()
        self.assertEqual(self.errors(), [])

    def test_missing_adr_0001(self):
        self.adr("ADR-0002-other.md", adr_text("ADR-0002"))
        self.assertIn("R-ADR-0001", rules(self.errors()))

    def test_missing_docs_adr(self):
        self.assertIn("R-ADR-0001", rules(cmd_check_adr.adr_errors(self.root)))

    def test_file_name_must_match_id_and_slug(self):
        self.good_tree()
        self.adr("ADR-0002-Bad_Slug.md", adr_text("ADR-0002"))
        self.adr("ADR-0003-ok.md", adr_text("ADR-0004"))
        found = [e for e in self.errors() if "R-ADR-NAME" in e]
        self.assertEqual(len(found), 2)

    def test_front_matter_errors(self):
        self.good_tree()
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", status="Maybe"))
        self.adr("ADR-0003-x.md", adr_text("ADR-0003").replace("date: 2026-10-17\n", ""))
        self.adr("ADR-0004-x.md", adr_text("ADR-0004", refs="[bogus]"))
        errors = self.errors()
        self.assertEqual(len([e for e in errors if "R-ADR-FRONT" in e]), 3)

    def test_section_order(self):
        self.good_tree()
        text = (
            adr_text("ADR-0002")
            .replace("## Context", "## TMP")
            .replace("## Decision", "## Context")
            .replace("## TMP", "## Decision")
        )
        self.adr("ADR-0002-x.md", text)
        self.assertIn("R-ADR-SECTION", rules(self.errors()))

    def test_citation_required(self):
        self.good_tree()
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", extra="\nIt SHOULD be quick.\n"))
        self.assertIn("R-ADR-CITE", rules(self.errors()))
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", extra="\nIt SHOULD be quick (machine fact: cmake 4.4.4).\n"))
        self.assertEqual(self.errors(), [])

    def test_length_limit(self):
        self.good_tree()
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", extra="x\n" * 80))
        self.assertIn("R-ADR-LENGTH", rules(self.errors()))

    def test_supersedes_must_resolve_and_target_must_be_superseded(self):
        self.good_tree()
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", supersedes="[ADR-0009]"))
        self.assertIn("R-ADR-SUPERSEDE", rules(self.errors()))
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", supersedes="[ADR-0001]"))
        self.assertIn("R-ADR-SUPERSEDE", rules(self.errors()))
        self.adr("ADR-0001-repo-layout.md", adr_text(status="Superseded"))
        self.assertEqual(self.errors(), [])
        self.adr("ADR-0002-x.md", adr_text("ADR-0002", supersedes="[ADR-0002]"))
        self.assertIn("R-ADR-SUPERSEDE", rules(self.errors()))

    def test_stale_log_fails(self):
        self.good_tree()
        self.assertEqual(self.errors(), [])
        self.adr("ADR-0002-x.md", adr_text("ADR-0002"))
        self.assertIn("R-ADR-LOG-STALE", rules(cmd_check_adr.adr_errors(self.root)))
        self.regenerate()
        self.assertEqual(cmd_check_adr.adr_errors(self.root), [])

    def test_missing_log_fails(self):
        self.good_tree()
        self.assertIn("R-ADR-LOG-STALE", rules(cmd_check_adr.adr_errors(self.root)))

    def test_repository_adrs_pass(self):
        self.assertEqual(cmd_check_adr.adr_errors(slgov.ROOT), [])

    def test_repository_adr_set(self):
        adrs = cmd_gen_adr_log.read_adrs(slgov.ROOT)
        ids = [a[0] for a in adrs]
        self.assertEqual(ids, sorted(ids))
        self.assertEqual(ids[:7], ["ADR-000%d" % i for i in range(1, 8)])  # the seven plan decisions come first
        by_id = {a[0]: a[2] for a in adrs}
        self.assertEqual(by_id["ADR-0001"]["decision_refs"], ["D-126", "D-135", "D-141", "D-144"])
        for meta in by_id.values():
            self.assertEqual(meta["status"], "Proposed")
        with open(
            os.path.join(slgov.ROOT, "docs", "adr", "ADR-0003-authoring-order-waivers.md"), encoding="utf-8"
        ) as fh:
            text = fh.read()
        self.assertIn("authoring-order waiver", text)
        self.assertIn("R-DEP-ACCEPTED", text)


class GeneratorTests(TreeCase):
    def test_render_is_deterministic_and_lists_every_adr(self):
        self.good_tree()
        self.adr("ADR-0002-x.md", adr_text("ADR-0002"))
        first = cmd_gen_adr_log.render(self.root)
        self.assertEqual(first, cmd_gen_adr_log.render(self.root))
        self.assertIn("[ADR-0001](../adr/ADR-0001-repo-layout.md)", first)
        self.assertIn("[ADR-0002](../adr/ADR-0002-x.md)", first)

    def test_generated_log_passes_doc_check(self):
        self.good_tree()
        self.regenerate()
        self.assertEqual(cmd_check_docs.doc_errors(self.root), [])

    def test_cli_writes_then_checks(self):
        self.good_tree()
        self.assertEqual(cmd_gen_adr_log.main(["x", "--root", self.root, "--check"]), 1)
        self.assertEqual(cmd_gen_adr_log.main(["x", "--root", self.root]), 0)
        self.assertEqual(cmd_gen_adr_log.main(["x", "--root", self.root, "--check"]), 0)

    def test_repository_log_is_current(self):
        with open(os.path.join(slgov.ROOT, *cmd_gen_adr_log.LOG_REL.split("/")), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), cmd_gen_adr_log.render(slgov.ROOT))


class TemplateTests(unittest.TestCase):
    def test_adr_template_front_matter_and_sections(self):
        path = os.path.join(slgov.ROOT, "docs", "templates", "ADR.md")
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        meta = slgov.parse_front_matter(text)
        for key in cmd_check_adr.REQUIRED_KEYS:
            self.assertIn(key, meta)
        self.assertEqual(cmd_check_adr.sections_error(text, "ADR.md"), [])


if __name__ == "__main__":
    unittest.main()
