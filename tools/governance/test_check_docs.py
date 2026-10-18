"""Unit tests for doc-check (cmd_check_docs.py)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_check_docs  # noqa: E402
import slgov  # noqa: E402

FIXTURES = os.path.join(slgov.ROOT, "tests", "governance", "docs")


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as fh:
        return fh.read()


def check(text, filename="SL-GOV-900-fixture.md"):
    return cmd_check_docs.check_document(text, "docs/gov/" + filename, filename)


def rules(errors):
    return {e.split(": ")[1] for e in errors}


class DocumentTests(unittest.TestCase):
    def test_good_fixture_passes(self):
        self.assertEqual(check(fixture("good.md")), [])

    def test_each_bad_fixture_fails_with_its_rule(self):
        names = sorted(n for n in os.listdir(FIXTURES) if n.startswith("bad-"))
        self.assertGreaterEqual(len(names), 5)
        for name in names:
            expected = name[len("bad-") : -len(".md")].upper()
            with self.subTest(name):
                self.assertIn(expected, rules(check(fixture(name))))

    def test_missing_key_is_front_error(self):
        text = fixture("good.md").replace("owner: founder\n", "")
        self.assertIn("R-DOC-FRONT", rules(check(text)))

    def test_kind_must_match_id(self):
        text = fixture("good.md").replace("kind: GOV", "kind: OPS")
        self.assertIn("R-DOC-NAME", rules(check(text)))

    def test_sections_out_of_order(self):
        text = fixture("good.md").replace("## Traceability", "## TMP").replace(
            "## Open issues", "## Traceability"
        ).replace("## TMP", "## Open issues")
        self.assertIn("R-DOC-SECTION", rules(check(text)))

    def test_extra_section_fails(self):
        self.assertIn("R-DOC-SECTION", rules(check(fixture("good.md") + "\n## Appendix\n")))

    def test_citation_forms(self):
        base = fixture("good.md")
        line = "- GOV900-R01: A change MUST cite its source (master plan section 6.3)."
        for cite in ("§6.3", "see section 6.3", "the master plan", "ADR-0004"):
            with self.subTest(cite):
                text = base.replace(line, "- GOV900-R01: A change SHOULD be small (%s)." % cite)
                self.assertEqual(check(text), [])
        text = base.replace(line, "- GOV900-R01: A change MAY be small.")
        self.assertIn("R-DOC-CITE", rules(check(text)))

    def test_fenced_lines_are_not_citation_checked(self):
        text = fixture("good.md")
        self.assertIn("MUST be valid", text)
        self.assertEqual(check(text), [])

    def test_lowercase_must_is_not_normative(self):
        text = fixture("good.md").replace("(master plan section 6.3)", "").replace("MUST", "must")
        self.assertEqual(check(text), [])

    def test_default_limit_is_150_lines(self):
        base = fixture("good.md").replace("None.\n", "None.\n" + "filler\n" * 150)
        self.assertIn("R-DOC-LENGTH", rules(check(base)))
        raised = base.replace("owner: founder", "owner: founder\nmax_lines: 400")
        self.assertEqual(check(raised), [])

    def test_unclosed_front_matter(self):
        self.assertIn("R-DOC-FRONT", rules(check("---\nid: SL-GOV-900\n")))

    def test_file_name_must_start_with_id(self):
        self.assertIn("R-DOC-NAME", rules(check(fixture("good.md"), "SL-GOV-901-x.md")))
        self.assertEqual(check(fixture("good.md"), "SL-GOV-900.md"), [])

    def test_template_front_matter_parses(self):
        path = os.path.join(slgov.ROOT, "docs", "templates", "DOC.md")
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        errors = cmd_check_docs.front_errors(slgov.parse_front_matter(text), "DOC.md", "SL-GOV-000.md")
        self.assertEqual(errors, [])
        self.assertEqual(cmd_check_docs.section_errors(text, "DOC.md"), [])


class TreeTests(unittest.TestCase):
    def write(self, root, rel, text):
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def test_master_plan_copy_and_readmes_are_exempt(self):
        with tempfile.TemporaryDirectory() as root:
            self.write(root, "docs/plan/SL-PLAN-001-master-plan.md", "# plan\nMUST nothing\n")
            self.write(root, "docs/gov/README.md", "# gov\nMUST nothing\n")
            self.assertEqual(cmd_check_docs.doc_errors(root), [])

    def test_other_plan_docs_are_checked(self):
        with tempfile.TemporaryDirectory() as root:
            self.write(root, "docs/plan/SL-PLAN-002-graph.md", "# no front matter\n")
            self.assertIn("R-DOC-FRONT", rules(cmd_check_docs.doc_errors(root)))

    def test_vol_and_gov_are_checked(self):
        with tempfile.TemporaryDirectory() as root:
            self.write(root, "docs/vol/SL-VOL-01-SPEC.md", "no front matter\n")
            self.write(root, "docs/gov/SL-GOV-900-fixture.md", fixture("good.md"))
            errors = cmd_check_docs.doc_errors(root)
            self.assertEqual(len(errors), 1)
            self.assertIn("docs/vol/SL-VOL-01-SPEC.md", errors[0])

    def test_arch_is_scanned_and_its_fixtures_fail_with_their_rule(self):
        arch = os.path.join(FIXTURES, "arch")
        names = sorted(n for n in os.listdir(arch) if n.startswith("bad-"))
        self.assertEqual(names, ["bad-r-doc-cite.md", "bad-r-doc-section.md"])
        with tempfile.TemporaryDirectory() as root:
            self.write(root, "docs/arch/README.md", "# arch\nMUST nothing\n")
            self.write(root, "docs/arch/SL-ARCH-900-fixture.md", fixture("arch/good.md"))
            self.assertEqual(cmd_check_docs.doc_errors(root), [])
            for name in names:
                with self.subTest(name):
                    self.write(root, "docs/arch/SL-ARCH-900-fixture.md", fixture("arch/" + name))
                    errors = cmd_check_docs.doc_errors(root)
                    self.assertIn(name[len("bad-") : -len(".md")].upper(), rules(errors))
                    self.assertTrue(all(e.startswith("docs/arch/SL-ARCH-900-fixture.md:") for e in errors))

    def test_arch_kind_must_match_directory_id(self):
        with tempfile.TemporaryDirectory() as root:
            text = fixture("arch/good.md").replace("kind: ARCH", "kind: GOV")
            self.write(root, "docs/arch/SL-ARCH-900-fixture.md", text)
            self.assertIn("R-DOC-NAME", rules(cmd_check_docs.doc_errors(root)))

    def test_repository_arch_documents_are_scanned(self):
        self.assertIn("docs/arch", cmd_check_docs.DOC_DIRS)
        for name in ("SL-ARCH-001-canonical-project-model.md", "SL-ARCH-002-transaction-undo-recovery.md"):
            with self.subTest(name):
                with open(os.path.join(slgov.ROOT, "docs", "arch", name), encoding="utf-8") as fh:
                    self.assertEqual(cmd_check_docs.check_document(fh.read(), "docs/arch/" + name, name), [])

    def test_repository_documents_pass(self):
        self.assertEqual(cmd_check_docs.doc_errors(slgov.ROOT), [])


if __name__ == "__main__":
    unittest.main()
