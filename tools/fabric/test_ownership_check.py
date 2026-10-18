"""Tests of the F2 ownership check (WO-048). FAB-INV-OWN-08: schema-file domains, the C++ dump and the domain_registry
table agree, one owner each; every planted disagreement fails naming its rule (and, for a second owner, both owners).

The dump is built here from SL-ARCH-001 exactly as sl_fabric_dump_domains writes it (the six fabric domains claimed by
02), so the tests need no C++ build; `just fabric-ownership-check` runs the check on the real dump.
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ownership_check  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def make_root(tmp):
    """A mini repository: the real SL-ARCH-001 and an empty schemas/fabric."""
    os.makedirs(os.path.join(tmp, "docs", "arch"))
    os.makedirs(os.path.join(tmp, "schemas", "fabric"))
    arch = sorted(n for n in os.listdir(os.path.join(REPO, "docs", "arch")) if n.startswith("SL-ARCH-001-"))[0]
    shutil.copy(os.path.join(REPO, "docs", "arch", arch), os.path.join(tmp, "docs", "arch", arch))
    return tmp


def make_dump(root):
    tables, errors = ownership_check.load_arch(root)
    assert not errors, errors
    fabric = next(r for r in tables["domain_registry"] if r["prefix"] == "fabric")
    domains = [{"name": d, "owner": "02", "state_classes": list(fabric["state_classes"]), "types": []}
               for d in sorted(tables["fabric_domains"])]
    return {
        "format": "sl-fabric-domains/1",
        "reserved": [dict(r) for r in tables["domain_registry"]],
        "fabric_domains": list(tables["fabric_domains"]),
        "actor_kinds": list(tables["actor_kinds"]),
        "default_deny": list(tables["default_deny"]),
        "domains": domains,
        "rejected": [],
        "f2": {"numerator": 0, "denominator": len(domains), "conflicts": 0},
    }


def module(name, domain, owner, types=()):
    return {"format": "sl-type-def/1", "module": name, "domain": domain, "owner": owner, "types": list(types)}


class OwnershipCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ownership-check-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = make_root(self.tmp)
        self.dump = make_dump(self.root)

    def add_module(self, file_name, doc):
        with open(os.path.join(self.root, "schemas", "fabric", file_name), "w", encoding="utf-8") as fh:
            json.dump(doc, fh)

    def run_check(self):
        path = os.path.join(self.root, "domains.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.dump, fh)
        return ownership_check.check(path, self.root)

    def assert_fails(self, rule, *texts):
        lines, _n, _d = self.run_check()
        hits = [line for line in lines if rule in line]
        self.assertTrue(hits, "expected %s in %r" % (rule, lines))
        for text in texts:
            self.assertTrue(any(text in line for line in hits), "expected %r in %r" % (text, hits))
        return hits

    def test_fab_inv_own_08_agree(self):
        lines, numerator, denominator = self.run_check()
        self.assertEqual(lines, [])
        self.assertEqual((numerator, denominator), (0, 6))
        # schema modules under their owners' prefixes agree with the dump and the table
        self.add_module("fabric.history.sltype.json", module("fabric.history", "fabric.history", "02",
                                                             [{"name": "fabric.history.revision"}]))
        self.add_module("content.scene.sltype.json", module("content.scene", "content.scene", "05",
                                                            [{"name": "content.scene.ui", "domain": "content.ui"}]))
        lines, numerator, denominator = self.run_check()
        self.assertEqual(lines, [])
        self.assertEqual((numerator, denominator), (0, 8))

    def test_fab_inv_own_08_real_repository_tables(self):
        # the real schema directory (whatever modules exist) agrees with a dump of the real table
        lines, numerator, _d = ownership_check.check(self._real_dump(), REPO)
        self.assertEqual([line for line in lines if "R-F2-DUP" in line or "R-F2-RESERVED" in line], [])
        self.assertEqual(numerator, 0)

    def _real_dump(self):
        path = os.path.join(self.tmp, "real.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(make_dump(REPO), fh)
        return path

    def test_fab_inv_own_08_second_owner_names_both(self):
        self.add_module("zz.dup.sltype.json", module("zz.dup", "fabric.history", "05"))
        hits = self.assert_fails("R-F2-DUP", "fabric.history", "owner 02", "owner 05")
        self.assertEqual(len(hits), 1)
        _lines, numerator, _d = self.run_check()
        self.assertEqual(numerator, 1)

    def test_fab_inv_own_08_two_modules_one_domain(self):
        self.add_module("a.sltype.json", module("content.a", "content.scene", "05"))
        self.add_module("b.sltype.json", module("sim.b", "content.scene", "06"))
        self.assert_fails("R-F2-DUP", "content.scene", "a.sltype.json owner 05", "b.sltype.json owner 06")

    def test_fab_inv_own_08_type_domain_of_another_owner(self):
        self.add_module("c.sltype.json", module("content.c", "content.c", "05",
                                                [{"name": "content.c.body", "domain": "sim.physics"}]))
        self.assert_fails("R-F2-DUP", "sim.physics", "owner 05", "owner 06")

    def test_fab_inv_own_08_reserved_table_drift(self):
        self.dump["reserved"][3]["owner"] = "05"
        self.assert_fails("R-F2-RESERVED", "gfx", "owner")
        self.dump = make_dump(self.root)
        self.dump["reserved"].pop()
        self.assert_fails("R-F2-RESERVED", "11 reserved prefixes")

    def test_fab_inv_own_08_tables_drift(self):
        self.dump["default_deny"] = ["agent"]
        self.assert_fails("R-F2-TABLES", "default_deny")
        self.dump = make_dump(self.root)
        self.dump["fabric_domains"].append("fabric.event")
        self.assert_fails("R-F2-TABLES", "fabric_domains")

    def test_fab_inv_own_08_orphan_and_state(self):
        self.add_module("ui.sltype.json", module("ui.hud", "ui.hud", "01"))
        self.assert_fails("R-F2-ORPHAN", "ui.hud")
        self.add_module("fx.sltype.json", module("fabric.extra", "fabric.extra", "02"))
        self.assert_fails("R-F2-ORPHAN", "fabric.extra", "within no domain the engine claims")
        self.dump["domains"][0]["state_classes"] = ["observation"]
        self.assert_fails("R-F2-STATE", "observation")

    def test_fab_inv_own_08_rejected_and_count(self):
        self.dump["rejected"] = [{"domain": "fabric.history", "claimant": "05", "holder": "02",
                                  "code": "ownership_conflict", "message": "refused"}]
        self.assert_fails("R-F2-REJECTED", "fabric.history", "holder 02")
        self.dump["f2"]["numerator"] = 1
        self.assert_fails("R-F2-COUNT", "numerator 1")

    def test_fab_inv_own_08_bad_inputs(self):
        self.dump = {"format": "other"}
        self.assert_fails("R-F2-DUMP", "sl-fabric-domains/1")
        self.dump = make_dump(self.root)
        with open(os.path.join(self.root, "schemas", "fabric", "bad.sltype.json"), "w", encoding="utf-8") as fh:
            fh.write("{")
        self.assert_fails("R-F2-ARCH", "bad.sltype.json")

    def test_fab_inv_own_08_main_exit_codes(self):
        path = os.path.join(self.root, "domains.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.dump, fh)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ownership_check.main([path, "--root", self.root]), 0)
            self.assertEqual(ownership_check.main([]), 2)
            self.add_module("zz.sltype.json", module("zz", "fabric.edge", "08"))
            self.assertEqual(ownership_check.main([path, "--root", self.root]), 1)
        text = out.getvalue()
        self.assertIn("f2 numerator 0 denominator 6", text)
        self.assertIn("f2 numerator 1 denominator 6", text)
        self.assertIn("fabric-ownership-check: failed", text)


if __name__ == "__main__":
    unittest.main()
