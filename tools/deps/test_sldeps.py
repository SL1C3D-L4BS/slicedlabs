"""Unit tests for sldeps (WO-017): schemas, derived state, check rules, lock, vendor, generated files."""

import contextlib
import copy
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sldeps_lib as lib  # noqa: E402

CMD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sldeps")


def copy_tree():
    tmp = tempfile.mkdtemp(prefix="sldeps-test-")
    for name in ("deps", "schemas", "third_party"):
        shutil.copytree(os.path.join(lib.ROOT, name), os.path.join(tmp, name))
    # These tests start from "doctest not vendored yet" (WO-017 state); WO-018 vendors it in the real tree.
    shutil.rmtree(os.path.join(tmp, "third_party", "doctest"), ignore_errors=True)
    lib.cmd_lock(tmp)
    return tmp


def edit_json(root, rel, fn):
    path = os.path.join(root, *rel.split("/"))
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    fn(doc)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(lib.dump_json(doc))


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


def rules(errors):
    return sorted({e.split(": ")[1] for e in errors})


class SldepsTest(unittest.TestCase):
    def setUp(self):
        self.root = copy_tree()
        self.addCleanup(shutil.rmtree, self.root, True)

    def test_committed_tree_is_clean(self):
        errors, _ = lib.check(lib.ROOT)
        self.assertEqual(errors, [])

    def test_lock_is_idempotent(self):
        before = read(self.root, "deps", "deps.lock.json")
        lib.cmd_lock(self.root)
        after = read(self.root, "deps", "deps.lock.json")
        self.assertEqual(before, after)
        self.assertEqual(lib.check(self.root)[0], [])

    def test_manifest_change_makes_lock_stale(self):
        edit_json(self.root, "deps/deps.json", lambda d: d["packages"]["doctest"].update(used_by=["sl_tests", "sl_core"]))
        errors, _ = lib.check(self.root)
        self.assertIn("R-DEPS-MANIFEST", rules(errors))
        lib.cmd_lock(self.root)
        self.assertEqual(lib.check(self.root)[0], [])

    def test_policy_drift(self):
        edit_json(self.root, "deps/deps.lock.json", lambda d: d["policy"]["licenses_allowed"].append("GPL-3.0-only"))
        self.assertIn("R-DEPS-POLICY", rules(lib.check(self.root)[0]))

    def test_vendored_hash_mismatch_names_the_file(self):
        path = os.path.join(self.root, "third_party", "cpm", "CPM.cmake")
        with open(path, "a", encoding="utf-8") as fh:
            fh.write("# tampered\n")
        errors, _ = lib.check(self.root)
        self.assertTrue(any("third_party/cpm/CPM.cmake" in e and "R-DEPS-HASH" in e for e in errors))

    def test_uncovered_file(self):
        stray = os.path.join(self.root, "third_party", "stray", "x.h")
        os.makedirs(os.path.dirname(stray))
        with open(stray, "w", encoding="utf-8") as fh:
            fh.write("//\n")
        errors, _ = lib.check(self.root)
        self.assertTrue(any("third_party/stray/x.h" in e and "R-DEPS-UNCOVERED" in e for e in errors))

    def test_licence_not_allowed(self):
        def bad(doc):
            doc["packages"]["doctest"]["license"] = "GPL-3.0-only"

        edit_json(self.root, "deps/deps.json", bad)
        lib.cmd_lock(self.root)
        self.assertIn("R-DEPS-LICENSE", rules(lib.check(self.root)[0]))

    def test_tier_b_needs_tested(self):
        edit_json(self.root, "deps/deps.lock.json", lambda d: d["packages"]["vulkan"]["providers"]["system"].update(tested=None))
        self.assertIn("R-DEPS-TIER", rules(lib.check(self.root)[0]))

    def test_unpinned_is_derived_and_strict_fails_on_first_in_order(self):
        errors, notes = lib.check(self.root)
        self.assertEqual(errors, [])
        self.assertIn("doctest via cpm is unpinned", notes)
        errors, _ = lib.check(self.root, strict=True, ci_order=["cpm", "vendored", "system", "vcpkg"])
        self.assertTrue(any("R-DEPS-UNPINNED" in e and "doctest via cpm" in e for e in errors))
        errors, _ = lib.check(self.root, strict=True, ci_order=["system", "vendored", "cpm", "vcpkg"])
        self.assertEqual(errors, [])

    def test_pending_and_unpinned_are_never_stored(self):
        text = read(lib.ROOT, "deps", "deps.lock.json")
        self.assertNotIn('"pending"', text)
        self.assertNotIn('"unpinned"', text)

    def test_provider_state(self):
        pkg = {"tier": "A"}
        self.assertEqual(lib.provider_state(pkg, "vendored", {"files": {}}), "unpinned")
        self.assertEqual(lib.provider_state(pkg, "vendored", {"files": {"a": "0" * 64}}), "pinned")
        self.assertEqual(lib.provider_state(pkg, "cpm", {"url_hash": None, "git_commit": None}), "unpinned")
        self.assertEqual(lib.provider_state(pkg, "cpm", {"url_hash": "0" * 64, "git_commit": None}), "pinned")
        self.assertEqual(lib.provider_state(pkg, "vcpkg", {"baseline": None}), "unpinned")
        self.assertEqual(lib.provider_state(pkg, "system", {"accept_sha256": None}), "unpinned")
        self.assertEqual(lib.provider_state({"tier": "B"}, "system", {"min": "1", "tested": "1"}), "pinned")

    def test_generated_vcpkg_drift_and_baseline(self):
        lib.cmd_sync_vcpkg(self.root)
        self.assertEqual(lib.check(self.root)[0], [])
        with open(os.path.join(self.root, "deps", "vcpkg", "vcpkg.json"), encoding="utf-8") as fh:
            doc = json.load(fh)
        self.assertNotIn("builtin-baseline", doc)
        self.assertNotIn("overrides", doc)
        with open(os.path.join(self.root, "deps", "vcpkg", "vcpkg.json"), "a", encoding="utf-8") as fh:
            fh.write(" ")
        self.assertIn("R-DEPS-GEN", rules(lib.check(self.root)[0]))
        edit_json(self.root, "deps/deps.lock.json", lambda d: d["packages"]["doctest"]["providers"]["vcpkg"].update(baseline="a" * 40))
        lib.cmd_sync_vcpkg(self.root)
        with open(os.path.join(self.root, "deps", "vcpkg", "vcpkg.json"), encoding="utf-8") as fh:
            doc = json.load(fh)
        self.assertEqual(doc["builtin-baseline"], "a" * 40)
        self.assertEqual(doc["overrides"], [{"name": "doctest", "version": "2.5.3"}])

    def test_vendor_from_system_pins_the_vendored_provider(self):
        include = os.path.join(self.root, "fake-include")
        os.makedirs(os.path.join(include, "doctest"))
        with open(os.path.join(include, "doctest", "doctest.h"), "w", encoding="utf-8") as fh:
            fh.write("// fake doctest\n")
        written = lib.cmd_vendor_from_system(self.root, "doctest", include)
        self.assertIn("third_party/doctest/doctest/doctest.h", written)
        self.assertIn("third_party/doctest/MANIFEST.json", written)
        errors, notes = lib.check(self.root)
        self.assertEqual(errors, [])
        self.assertNotIn("doctest via vendored is unpinned", notes)
        self.assertEqual(lib.check(self.root, strict=True)[0], [])

    def test_vendor_refuses_missing_header(self):
        with self.assertRaises(lib.DepsError):
            lib.cmd_vendor_from_system(self.root, "doctest", os.path.join(self.root, "nowhere"))
        with self.assertRaises(lib.DepsError):
            lib.cmd_vendor_from_system(self.root, "vulkan")

    def test_lock_refresh_pins_cpm_url_hash_from_a_downloaded_file(self):
        archive = os.path.join(self.root, "doctest.tar.gz")
        with open(archive, "wb") as fh:
            fh.write(b"fake archive\n")
        self.assertEqual(lib.cmd_lock_refresh(self.root, "doctest", archive), ["deps/deps.lock.json"])
        prov = lib.load(self.root, lib.LOCK_REL)["packages"]["doctest"]["providers"]["cpm"]
        self.assertEqual(prov["url_hash"], lib.sha256_file(archive))
        self.assertIsNone(prov["git_commit"])
        self.assertEqual(lib.check(self.root)[0], [])
        lib.cmd_lock(self.root)  # `sldeps lock` keeps the pin
        prov = lib.load(self.root, lib.LOCK_REL)["packages"]["doctest"]["providers"]["cpm"]
        self.assertEqual(prov["url_hash"], lib.sha256_file(archive))
        with self.assertRaises(lib.DepsError):
            lib.cmd_lock_refresh(self.root, "doctest", os.path.join(self.root, "nowhere.tar.gz"))
        with self.assertRaises(lib.DepsError):
            lib.cmd_lock_refresh(self.root, "vulkan", archive)
        with self.assertRaises(lib.DepsError):
            lib.cmd_lock_refresh(self.root, "nosuchpkg", archive)

    def test_cpm_url_is_derived_from_the_github_tag(self):
        self.assertEqual(lib.cpm_url(self.root, "doctest"), "https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz")
        with self.assertRaises(lib.DepsError):
            lib.cpm_url(self.root, "vulkan")

    def test_schema_violation(self):
        edit_json(self.root, "deps/deps.json", lambda d: d["packages"]["doctest"]["providers"].update(conan={}))
        self.assertIn("R-DEPS-SCHEMA", rules(lib.check(self.root)[0]))

    def test_register_table(self):
        lock = lib.load(self.root, lib.LOCK_REL)
        table = lib.register_table(copy.deepcopy(lock))
        self.assertIn("| doctest | 2.5.3 | MIT | test | A |", table)
        self.assertIn("vendored (unpinned)", table)

    def test_cli_exit_codes(self):
        self.assertEqual(lib.cmd_lock(self.root)[0], "deps/deps.lock.json")
        loader = importlib.machinery.SourceFileLoader("sldeps_cli", CMD)
        spec = importlib.util.spec_from_loader("sldeps_cli", loader)
        cli = importlib.util.module_from_spec(spec)
        loader.exec_module(cli)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cli.main(["sldeps", "check", "--root", self.root]), 0)
            self.assertEqual(cli.main(["sldeps", "check", "--strict", "--root", self.root]), 1)
            self.assertEqual(cli.main(["sldeps", "bogus"]), 2)
            self.assertEqual(cli.main(["sldeps"]), 2)


if __name__ == "__main__":
    unittest.main()
