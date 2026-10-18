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
import tarfile
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sldeps_lib as lib  # noqa: E402
import sldeps_supply as supply  # noqa: E402

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


def edit_lock(root, fn):
    edit_json(root, "deps/deps.lock.json", fn)


def vend(doc, pkg):
    return doc["packages"][pkg]["providers"]["vendored"]


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
        used = ["sl_tests", "sl_core"]
        edit_json(self.root, "deps/deps.json", lambda d: d["packages"]["doctest"].update(used_by=used))
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
        edit_lock(self.root, lambda d: d["packages"]["vulkan"]["providers"]["system"].update(tested=None))
        self.assertIn("R-DEPS-TIER", rules(lib.check(self.root)[0]))

    def test_unpinned_is_derived_and_strict_fails_on_first_in_order(self):
        errors, notes = lib.check(self.root)
        self.assertEqual(errors, [])
        self.assertIn("doctest via vcpkg is unpinned", notes)  # cpm was pinned by deps-lock-refresh (2026-10-17)
        errors, _ = lib.check(self.root, strict=True, ci_order=["vcpkg", "vendored", "system", "cpm"])
        self.assertTrue(any("R-DEPS-UNPINNED" in e and "doctest via vcpkg" in e for e in errors))
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
        edit_lock(self.root, lambda d: d["packages"]["doctest"]["providers"]["vcpkg"].update(baseline="a" * 40))
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
        want = "https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz"
        self.assertEqual(lib.cpm_url(self.root, "doctest"), want)
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


def full_tree():
    """A copy of the committed tree with doctest and cpm vendored, licensed and attested (WO-032)."""
    tmp = tempfile.mkdtemp(prefix="sldeps-supply-")
    for name in ("deps", "schemas", "third_party"):
        shutil.copytree(os.path.join(lib.ROOT, name), os.path.join(tmp, name))
    return tmp


def write_json(root, rel, doc):
    with open(os.path.join(root, *rel.split("/")), "w", encoding="utf-8") as fh:
        fh.write(lib.dump_json(doc))


def finding(**extra):
    doc = {"package": "cpm", "version": "0.43.2", "advisory": "CVE-0000-0000", "status": "open", "source": "OSV"}
    doc.update(date="2026-10-17")
    doc.update(extra)
    return doc


class SupplyTest(unittest.TestCase):
    """WO-032: licences, upstream attestation and the vulnerability register."""

    def setUp(self):
        self.root = full_tree()
        self.addCleanup(shutil.rmtree, self.root, True)

    def lock(self):
        return lib.load(self.root, lib.LOCK_REL)

    def test_committed_tree_passes_every_rule(self):
        lock = lib.load(lib.ROOT, lib.LOCK_REL)
        self.assertEqual(supply.licenses(lib.ROOT, lock), [])
        self.assertEqual(supply.attest_check(lib.ROOT, lock), [])
        self.assertEqual(supply.vuln_check(lib.ROOT, lock), [])

    def test_licence_outside_the_allowlist_or_not_spdx(self):
        edit_json(self.root, "deps/deps.lock.json", lambda d: d["packages"]["doctest"].update(license="GPL-3.0-only"))
        errors = supply.licenses(self.root, self.lock())
        self.assertEqual(rules(errors), ["R-DEPS-LICENSE"])
        self.assertIn("doctest", errors[0])
        edit_json(self.root, "deps/deps.lock.json", lambda d: d["packages"]["doctest"].update(license="MIT OR GPL-2.0"))
        self.assertIn("plain SPDX", supply.licenses(self.root, self.lock())[0])

    def test_vendored_dir_needs_a_manifested_licence_file(self):
        edit_lock(self.root, lambda d: vend(d, "cpm")["files"].pop("LICENSE.txt"))
        errors = supply.licenses(self.root, self.lock())
        self.assertEqual(rules(errors), ["R-DEPS-LICFILE"])
        self.assertIn("cpm", errors[0])

    def test_licence_file_must_carry_the_licence_text(self):
        with open(os.path.join(self.root, "third_party", "cpm", "LICENSE.txt"), "w", encoding="utf-8") as fh:
            fh.write("TODO\n")
        self.assertIn("does not look like the MIT text", supply.licenses(self.root, self.lock())[0])

    def test_unverified_needs_a_valid_waiver(self):
        edit_lock(self.root, lambda d: vend(d, "cpm").update(verified_upstream=False))
        errors = supply.attest_check(self.root, self.lock(), "2026-10-17")
        self.assertEqual(rules(errors), ["R-DEPS-ATTEST"])
        self.assertIn("cpm", errors[0])
        waiver = {"package": "cpm", "reason": "local cache only", "expires": "2026-11-01"}
        write_json(self.root, supply.WAIVERS_REL, {"schema": supply.WAIVERS_SCHEMA, "waivers": [waiver]})
        self.assertEqual(supply.attest_check(self.root, self.lock(), "2026-11-01"), [])
        self.assertIn("expired on 2026-11-01", supply.attest_check(self.root, self.lock(), "2026-11-02")[0])
        for bad in ({"reason": ""}, {"expires": "soon"}):
            doc = {"schema": supply.WAIVERS_SCHEMA, "waivers": [dict(waiver, **bad)]}
            write_json(self.root, supply.WAIVERS_REL, doc)
            self.assertEqual(rules(supply.attest_check(self.root, self.lock(), "2026-10-17")), ["R-DEPS-ATTEST"])

    def test_verified_needs_a_matching_attestation(self):
        attest = os.path.join(self.root, *supply.ATTEST_REL.split("/"))
        saved = read(attest)
        os.remove(attest)
        self.assertEqual(rules(supply.attest_check(self.root, self.lock())), ["R-DEPS-ATTEST-STALE"])
        with open(attest, "w", encoding="utf-8") as fh:  # restore, then change a vendored hash in the lock only
            fh.write(saved)
        self.assertEqual(supply.attest_check(self.root, self.lock()), [])
        edit_lock(self.root, lambda d: vend(d, "cpm")["files"].update({"CPM.cmake": "1" * 64}))
        self.assertEqual(rules(supply.attest_check(self.root, self.lock())), ["R-DEPS-ATTEST-STALE"])

    def archive_of_doctest(self, mutate=None):
        path = os.path.join(self.root, "doctest-2.5.3.tar.gz")
        with tarfile.open(path, "w:gz") as tf:
            for rel in ("doctest/doctest.h", "LICENSE.txt"):
                src = os.path.join(self.root, "third_party", "doctest", *rel.split("/"))
                tf.add(src, arcname="doctest-2.5.3/" + rel)
            if mutate:
                mutate(tf)
        return path

    def unattest(self, pkg):
        edit_lock(self.root, lambda d: vend(d, pkg).update(verified_upstream=False))
        edit_json(self.root, supply.ATTEST_REL, lambda d: d["packages"].pop(pkg))

    def test_attest_from_a_tar_archive_sets_verified_and_records_the_sha(self):
        archive = self.archive_of_doctest()
        self.unattest("doctest")
        self.assertEqual(rules(supply.attest_check(self.root, self.lock())), ["R-DEPS-ATTEST"])
        written = lib.cmd_attest(self.root, "doctest", archive)
        self.assertIn("deps/attestations.json", written)
        prov = self.lock()["packages"]["doctest"]["providers"]["vendored"]
        self.assertTrue(prov["verified_upstream"])
        record = lib.load(self.root, supply.ATTEST_REL)["packages"]["doctest"]
        self.assertEqual(record["upstream_archive_sha256"], lib.sha256_file(archive))
        self.assertEqual(sorted(record["files"]), ["LICENSE.txt", "doctest/doctest.h"])
        self.assertEqual(supply.attest_check(self.root, self.lock()), [])
        self.assertEqual(lib.check(self.root)[0], [])  # the generated MANIFEST.json follows the lock

    def test_attest_refuses_a_file_that_differs_from_upstream(self):
        archive = self.archive_of_doctest()
        self.unattest("doctest")
        header = os.path.join(self.root, "third_party", "doctest", "doctest", "doctest.h")
        with open(header, "a", encoding="utf-8") as fh:
            fh.write("// tampered\n")
        lib.cmd_lock(self.root)
        with self.assertRaises(lib.DepsError) as ctx:
            lib.cmd_attest(self.root, "doctest", archive)
        self.assertIn("doctest/doctest.h differs from upstream", str(ctx.exception))
        self.assertFalse(self.lock()["packages"]["doctest"]["providers"]["vendored"]["verified_upstream"])

    def test_attest_refuses_a_file_missing_from_the_archive(self):
        path = os.path.join(self.root, "empty.tar.gz")
        with tarfile.open(path, "w:gz") as tf:
            licence = os.path.join(self.root, "third_party", "doctest", "LICENSE.txt")
            tf.add(licence, arcname="doctest-2.5.3/LICENSE.txt")
        self.unattest("doctest")
        with self.assertRaises(lib.DepsError) as ctx:
            lib.cmd_attest(self.root, "doctest", path)
        self.assertIn("is not in the archive", str(ctx.exception))

    def test_attest_a_bare_release_file(self):
        self.unattest("cpm")
        archive = os.path.join(self.root, "CPM.cmake.download")
        shutil.copyfile(os.path.join(self.root, "third_party", "cpm", "CPM.cmake"), archive)
        lib.cmd_attest(self.root, "cpm", archive)
        self.assertEqual(supply.attest_check(self.root, self.lock()), [])
        self.assertEqual(sorted(lib.load(self.root, supply.ATTEST_REL)["packages"]["cpm"]["files"]), ["CPM.cmake"])
        with open(archive, "a", encoding="utf-8") as fh:
            fh.write("# other\n")
        with self.assertRaises(lib.DepsError):
            lib.cmd_attest(self.root, "cpm", archive)

    def test_attest_errors(self):
        with self.assertRaises(lib.DepsError):
            lib.cmd_attest(self.root, "vulkan", "x")
        with self.assertRaises(lib.DepsError):
            lib.cmd_attest(self.root, "cpm", os.path.join(self.root, "nowhere"))

    def test_empty_register_passes_and_open_finding_on_a_locked_version_fails(self):
        self.assertEqual(supply.vuln_check(self.root, self.lock()), [])
        write_json(self.root, supply.VULNS_REL, {"schema": supply.VULNS_SCHEMA, "findings": [finding()]})
        errors = supply.vuln_check(self.root, self.lock())
        self.assertEqual(rules(errors), ["R-DEPS-VULN"])
        self.assertIn("CVE-0000-0000", errors[0])
        self.assertIn("R-DEPS-VULN", rules(lib.check(self.root)[0]))  # folded into `sldeps check`

    def test_findings_that_do_not_fail(self):
        others = [finding(version="0.40.0"), finding(package="notlocked")]
        others += [finding(status="fixed"), finding(status="wontfix")]
        write_json(self.root, supply.VULNS_REL, {"schema": supply.VULNS_SCHEMA, "findings": others})
        self.assertEqual(supply.vuln_check(self.root, self.lock()), [])

    def test_wildcard_version_and_tier_b_tested_version(self):
        write_json(self.root, supply.VULNS_REL, {"schema": supply.VULNS_SCHEMA, "findings": [finding(version="*")]})
        self.assertEqual(rules(supply.vuln_check(self.root, self.lock())), ["R-DEPS-VULN"])
        tier_b = finding(package="vulkan", version=self.lock()["packages"]["vulkan"]["providers"]["system"]["tested"])
        write_json(self.root, supply.VULNS_REL, {"schema": supply.VULNS_SCHEMA, "findings": [tier_b]})
        self.assertEqual(rules(supply.vuln_check(self.root, self.lock())), ["R-DEPS-VULN"])

    def test_malformed_register(self):
        for doc in (
            {"schema": "sl-vulns/2", "findings": []},
            {"schema": supply.VULNS_SCHEMA, "findings": [finding(status="maybe")]},
            {"schema": supply.VULNS_SCHEMA, "findings": [finding(date="yesterday")]},
            {"schema": supply.VULNS_SCHEMA, "findings": [{"package": "cpm"}]},
        ):
            write_json(self.root, supply.VULNS_REL, doc)
            self.assertEqual(rules(supply.vuln_check(self.root, self.lock())), ["R-DEPS-VULN"])
        os.remove(os.path.join(self.root, *supply.VULNS_REL.split("/")))
        self.assertEqual(rules(supply.vuln_check(self.root, self.lock())), ["R-DEPS-VULN"])

    def test_cli_rules_exit_codes(self):
        loader = importlib.machinery.SourceFileLoader("sldeps_cli2", CMD)
        spec = importlib.util.spec_from_loader("sldeps_cli2", loader)
        cli = importlib.util.module_from_spec(spec)
        loader.exec_module(cli)
        root = ["--root", self.root]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            for cmd in ("licenses", "attest-check", "vuln-check", "check"):
                self.assertEqual(cli.main(["sldeps", cmd] + root), 0, cmd)
            self.assertEqual(cli.main(["sldeps", "attest", "--from-archive", "cpm"] + root), 2)
            edit_lock(self.root, lambda d: vend(d, "cpm").update(verified_upstream=False))
            self.assertEqual(cli.main(["sldeps", "attest-check"] + root), 1)
            self.assertEqual(cli.main(["sldeps", "attest-check", "--today", "2026-10-17"] + root), 1)


if __name__ == "__main__":
    unittest.main()
