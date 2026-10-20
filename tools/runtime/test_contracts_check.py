"""Tests for the runtime contracts checker (WO-077): a control fixture and one planted defect per rule.

The fixture is a temp mini repository built from strings (no subprocess): a SL-ARCH-003 stand-in with three
invariants, two work orders, a justfile with a real, a stub and two dispatch recipes (one real script, one stub script),
recipes.json, a C++ test and a Python test.
"""

import contextlib
import copy
import importlib.util
import io
import json
import os
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("runtime_contracts_check", os.path.join(_here, "contracts_check.py"))
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)

REPO = os.path.dirname(os.path.dirname(_here))
STUB = "#!/usr/bin/env bash\nexec python3 -I tools/governance/cmd_stub.py %s\n"
REAL = "#!/usr/bin/env bash\nexec python3 -I tools/runtime/contracts_check.py\n"

ROWS = [
    {"id": "RT-INV-ERR-01", "rule": "ARCH3-R017", "text": "codes are unique",
     "test": "tests/unit/runtime/err_codes_test.cpp", "wo": "WO-001", "verified_by": "just real"},
    {"id": "RT-INV-LINT-01", "rule": "ARCH3-R009", "text": "lint rejects", "test": "tools/runtime/test_fixture.py",
     "wo": "WO-002", "verified_by": "just runtime-lint"},
    {"id": "RT-INV-ERR-02", "rule": "ARCH3-R017", "text": "domain", "test": "tests/unit/runtime/err_domain_test.cpp",
     "wo": "WO-001", "verified_by": "just real"},
]

DOC = """---
id: SL-ARCH-003
---
# fixture

Rule ids are ARCH3-Rnnn; invariants are RT-INV-<AREA>-nn with AREA one of ERR LINT
STRESS GATE, and a C++ test file starts with the lower-case area.

- ARCH3-R009: A header MUST NOT include a higher layer.
- ARCH3-R014: Every row MUST bind to one test file.
- ARCH3-R017: Every fallible call returns a result.

```sl-data
%s
```
"""

JUSTFILE = """default:
    @echo ok

real:
    @echo real

stubby:
    @just stub stubby

runtime-lint *args:
    @bash scripts/runtime/runtime-lint.sh {{args}}

runtime-test *args:
    @bash scripts/runtime/runtime-test.sh {{args}}
"""

RECIPES = {"schema": "sl-recipes/1", "recipes": {
    "default": {"owner": "WO-001", "status": "implemented"},
    "real": {"owner": "WO-001", "status": "implemented"},
    "stubby": {"owner": "WO-002", "status": "stub"},
    "runtime-lint": {"owner": "WO-002", "status": "dispatch"},
    "runtime-test": {"owner": "WO-001", "status": "dispatch"},
}}


def work_order(rows, wo):
    return "---\nid: %s\n---\nInvariants owned:\n%s\n" % (
        wo, "\n".join("- %s (%s): %s [%s]" % (r["id"], r["rule"], r["text"], r["test"]) for r in rows if r["wo"] == wo))


def build(root, rows=None, files=None, doc_rows=None):
    """Write the control fixture under root; `rows` is what the table holds, `files` overrides or adds files."""
    rows = copy.deepcopy(ROWS if rows is None else rows)
    table = json.dumps({"invariants": rows}, indent=0)
    out = {
        "docs/arch/SL-ARCH-003-fixture.md": DOC % table,
        "work-orders/WO-001.md": work_order(ROWS, "WO-001"),
        "work-orders/WO-002.md": work_order(ROWS, "WO-002"),
        "justfile": JUSTFILE,
        "tools/governance/recipes.json": json.dumps(RECIPES),
        "scripts/runtime/runtime-lint.sh": REAL,
        "scripts/runtime/runtime-test.sh": STUB % "runtime-test",
        "tests/unit/runtime/err_codes_test.cpp": 'TEST_CASE("RT-INV-ERR-01: codes are unique") {}\n',
        "tests/unit/runtime/err_domain_test.cpp": 'TEST_CASE("RT-INV-ERR-02: domain") {}\n',
        "tools/runtime/test_fixture.py": "class T:\n    def test_rt_inv_lint_01_rejects(self):\n        pass\n",
    }
    out.update(files or {})
    for rel, text in out.items():
        if text is None:
            continue
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
    return root


@contextlib.contextmanager
def tree(**kw):
    with tempfile.TemporaryDirectory() as tmp:
        yield build(tmp, **kw)


def run(root, strict=False):
    out = io.StringIO()
    argv = ["contracts_check.py", "--root", root] + (["--strict"] if strict else [])
    with contextlib.redirect_stdout(out):
        status = cc.main(argv)
    return status, out.getvalue()


def row(**changes):
    r = dict(ROWS[0])
    r.update(changes)
    return r


class ContractsCheckTest(unittest.TestCase):
    def named(self, out, rule, rid):
        return [line for line in out.splitlines() if rule in line and rid in line and not line.startswith("pending")]

    def refused(self, rule, rid, **kw):
        with tree(**kw) as root:
            for strict in (False, True):
                status, out = run(root, strict)
                self.assertEqual(status, 1, "%s strict=%s:\n%s" % (rule, strict, out))
                self.assertTrue(self.named(out, rule, rid), "no line names %s and %s:\n%s" % (rule, rid, out))

    # --- RT-INV-LINT-03 ---------------------------------------------------------------------------------------

    def test_rt_inv_lint_03_control_binds_both_ways(self):
        with tree() as root:
            for strict in (False, True):
                status, out = run(root, strict)
                self.assertEqual(status, 0, out)
                self.assertTrue(out.rstrip().endswith("invariants 3 bound 3 pending 0"), out)
        self.assertEqual(run_name(False), "runtime-contracts: ok")

    def test_rt_inv_lint_03_orphan_test(self):
        self.refused("R-RT-ORPHAN-TEST", "RT-INV-ZZ-99",
                     files={"tests/unit/runtime/zz_orphan_test.cpp": 'TEST_CASE("RT-INV-ZZ-99: planted orphan") {}\n'})
        self.refused("R-RT-ORPHAN-TEST", "RT-INV-ERR-07",
                     files={"tools/runtime/test_other.py": "def test_rt_inv_err_07_planted(self):\n    pass\n"})

    def test_rt_inv_lint_03_duplicate_id(self):
        self.refused("R-RT-DUP", "RT-INV-ERR-01", rows=ROWS + [row(wo="WO-002")])

    def test_rt_inv_lint_03_unknown_work_order(self):
        self.refused("R-RT-WO", "RT-INV-ERR-02", rows=[ROWS[0], ROWS[1], dict(ROWS[2], wo="WO-999")])

    def test_rt_inv_lint_03_work_order_does_not_list_the_id(self):
        listing = {"work-orders/WO-001.md": "---\nid: WO-001\n---\n- RT-INV-ERR-01 (x)\n"}
        self.refused("R-RT-WO", "RT-INV-ERR-02", files=listing)

    def test_rt_inv_lint_03_work_order_lists_an_unknown_or_foreign_id(self):
        self.refused("R-RT-WO", "RT-INV-ERR-09", files={
            "work-orders/WO-002.md": work_order(ROWS, "WO-002") + "- RT-INV-ERR-09 (ARCH3-R017): planted\n"})
        self.refused("R-RT-WO", "RT-INV-ERR-01", files={
            "work-orders/WO-002.md": work_order(ROWS, "WO-002") + "- RT-INV-ERR-01 (ARCH3-R017): planted\n"})

    def test_rt_inv_lint_03_wrong_location(self):
        self.refused("R-RT-LOCATION", "RT-INV-ERR-01", files={
            "tests/integration/runtime/err_second_test.cpp": 'TEST_CASE("RT-INV-ERR-01: elsewhere") {}\n'})
        self.refused("R-RT-LOCATION", "RT-INV-LINT-01", files={
            "tools/runtime/test_second.py": "def test_rt_inv_lint_01_elsewhere(self):\n    pass\n"})

    def test_rt_inv_lint_03_rule_area_shape_and_per_wo(self):
        self.refused("R-RT-RULE", "RT-INV-ERR-01", rows=[row(rule="ARCH3-R555")] + ROWS[1:])
        wrong = "tests/unit/runtime/mem_codes_test.cpp"
        self.refused("R-RT-AREA", "RT-INV-ERR-01", rows=[row(test=wrong)] + ROWS[1:],
                     files={wrong: 'TEST_CASE("RT-INV-ERR-01: codes") {}\n',
                            "tests/unit/runtime/err_codes_test.cpp": None})
        self.refused("R-RT-SHAPE", "RT-INV-ERR-01", rows=[dict(row(), extra="x")] + ROWS[1:])
        self.refused("R-RT-SHAPE", "RT-INV-NOPE-01", rows=[row(id="RT-INV-NOPE-01")] + ROWS[1:])
        self.refused("R-RT-SHAPE", "RT-INV-ERR-01", rows=[row(test="docs/nowhere.md")] + ROWS[1:])
        many = [dict(ROWS[0], id="RT-INV-ERR-%02d" % n) for n in range(1, 10)]
        with tree(rows=many) as root:
            status, out = run(root)
            self.assertEqual(status, 1)
            self.assertTrue(self.named(out, "R-RT-PER-WO", "WO-001"), out)

    def test_rt_inv_lint_03_unknown_recipe(self):
        self.refused("R-RT-RECIPE", "RT-INV-ERR-01", rows=[row(verified_by="just nowhere")] + ROWS[1:])

    def test_rt_inv_lint_03_missing_document_and_usage(self):
        with tree(files={"docs/arch/SL-ARCH-003-fixture.md": "# no table\n"}) as root:
            status, out = run(root)
            self.assertEqual(status, 1)
            self.assertIn("R-RT-SHAPE", out)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cc.main(["contracts_check.py", "--nope"]), 2)

    def test_rt_inv_lint_03_repository_table_is_consistent(self):
        errors, pending, (total, bound, waiting) = cc.check(REPO)
        self.assertEqual(errors, [])
        self.assertGreater(total, 0)
        self.assertEqual(bound + waiting, total)
        self.assertEqual(len({p[2] for p in pending}), waiting)

    # --- RT-INV-LINT-04 ---------------------------------------------------------------------------------------

    def test_rt_inv_lint_04_missing_test_is_pending_then_an_error(self):
        with tree(files={"tests/unit/runtime/err_domain_test.cpp": None}) as root:
            status, out = run(root)
            self.assertEqual(status, 0, out)
            self.assertIn("pending R-RT-MISSING-TEST WO-001 (1): RT-INV-ERR-02", out)
            self.assertTrue(out.rstrip().endswith("invariants 3 bound 2 pending 1"), out)
            status, out = run(root, strict=True)
            self.assertEqual(status, 1, out)
            self.assertTrue(self.named(out, "R-RT-MISSING-TEST", "RT-INV-ERR-02"), out)
            self.assertTrue(out.rstrip().endswith("invariants 3 bound 2 pending 1"), out)

    def test_rt_inv_lint_04_dispatch_stub_script_is_pending(self):
        rows = [ROWS[0], dict(ROWS[1], verified_by="just runtime-test err"), ROWS[2]]
        with tree(rows=rows) as root:
            status, out = run(root)
            self.assertEqual(status, 0, out)
            self.assertIn("pending R-RT-RECIPE WO-002 (1): RT-INV-LINT-01", out)
            self.assertTrue(out.rstrip().endswith("pending 1"), out)
            status, out = run(root, strict=True)
            self.assertEqual(status, 1, out)
            self.assertTrue(self.named(out, "R-RT-RECIPE", "RT-INV-LINT-01"), out)
        # the same row is bound once the script is real
        with tree(rows=rows, files={"scripts/runtime/runtime-test.sh": REAL}) as root:
            for strict in (False, True):
                status, out = run(root, strict)
                self.assertEqual(status, 0, out)
                self.assertTrue(out.rstrip().endswith("invariants 3 bound 3 pending 0"), out)

    def test_rt_inv_lint_04_stub_recipe_is_pending(self):
        rows = [dict(ROWS[0], verified_by="just stubby")] + ROWS[1:]
        with tree(rows=rows) as root:
            status, out = run(root)
            self.assertEqual(status, 0, out)
            self.assertIn("pending R-RT-RECIPE", out)
            status, out = run(root, strict=True)
            self.assertEqual(status, 1, out)
            self.assertTrue(self.named(out, "R-RT-RECIPE", "RT-INV-ERR-01"), out)

    def test_rt_inv_lint_04_a_missing_test_file_and_stub_count_once(self):
        rows = [ROWS[0], dict(ROWS[1], verified_by="just runtime-test err"), ROWS[2]]
        with tree(rows=rows, files={"tools/runtime/test_fixture.py": "\n"}) as root:
            status, out = run(root)
            self.assertEqual(status, 0, out)
            self.assertTrue(out.rstrip().endswith("invariants 3 bound 2 pending 1"), out)


def run_name(strict):
    with tree() as root:
        return run(root, strict)[1].splitlines()[-2]


if __name__ == "__main__":
    unittest.main()
