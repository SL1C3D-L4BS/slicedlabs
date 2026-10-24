"""Unit tests for tools/ci/check_workflows.py, check_toolchain.py and miniyaml.py (WO-028, WO-130)."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_toolchain  # noqa: E402
import check_workflows  # noqa: E402
import miniyaml  # noqa: E402

SHA = "a" * 39 + "b"
GOOD = f"""name: t
on:
  push:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@{SHA}  # v4.2.2
      - name: build
        run: |
          echo hi
          bash scripts/ci-local.sh
"""
DEPENDABOT = """version: 2
updates:
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "monthly"
"""
DIGEST = "sha256:" + "ab" * 32
IMAGE = f"ghcr.io/archlinux/archlinux:base-devel@{DIGEST}"
PIN = {
    "schema": "sl-ci-linux/1",
    "image": {"name": "ghcr.io/archlinux/archlinux", "tag": "base-devel", "digest": DIGEST},
    "archive": {"date": "2026/10/23", "server": "https://archive.archlinux.org/repos/2026/10/23/$repo/os/$arch"},
    "packages": {"clang": "23.1.3-4", "vulkan-headers": "1:1.4.363.0-1"},
}
PRESETS = {
    "configurePresets": [{"name": "debug"}, {"name": "hid", "hidden": True}],
    "workflowPresets": [{"name": "ci-clang"}],
}


class MiniYamlTest(unittest.TestCase):
    def test_nested_structures(self):
        doc = miniyaml.loads(
            "a:\n  b: [x, y]\n  c: {}\nl:\n  - one\n  - k: v\n    w: 'q: r'  # note\nt: |\n  line # kept\n  two\n"
        )
        self.assertEqual(list(doc["a"]["b"]), ["x", "y"])
        self.assertEqual(doc["a"]["c"], {})
        self.assertEqual(doc["l"][0], "one")
        self.assertEqual((doc["l"][1]["k"], doc["l"][1]["w"]), ("v", "q: r"))
        self.assertEqual(doc["t"], "line # kept\ntwo\n")

    def test_list_at_key_indent(self):
        self.assertEqual(list(miniyaml.loads("a:\n- x\n- y\nb: 1\n")["a"]), ["x", "y"])

    def test_errors(self):
        for text in ("a: 1\na: 2\n", "a:\n\t- x\n", "a: &x 1\n"):
            with self.assertRaises(miniyaml.YamlError):
                miniyaml.loads(text)


class CheckToolchainTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.pin_path = os.path.join(self._tmp.name, "ci-linux.json")
        self.q_path = os.path.join(self._tmp.name, "q.txt")

    def tearDown(self):
        self._tmp.cleanup()

    def run_verify(self, installed, pin=None):
        with open(self.pin_path, "w", encoding="utf-8") as handle:
            json.dump(PIN if pin is None else pin, handle)
        with open(self.q_path, "w", encoding="utf-8") as handle:
            handle.write(installed)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = check_toolchain.main(["verify", self.pin_path, self.q_path])
        return rc, out.getvalue()

    def test_matching_versions_pass(self):
        rc, out = self.run_verify("bash 5.3-1\nclang 23.1.3-4\nvulkan-headers 1:1.4.363.0-1\n")
        self.assertEqual(rc, 0, out)

    def test_version_drift_and_missing_package_fail_and_name_the_package(self):
        rc, out = self.run_verify("clang 23.1.4-1\nvulkan-headers 1:1.4.363.0-1\n")
        self.assertEqual(rc, 1)
        self.assertIn("clang: installed 23.1.4-1, pinned 23.1.3-4", out)
        rc, out = self.run_verify("clang 23.1.3-4\n")
        self.assertEqual(rc, 1)
        self.assertIn("vulkan-headers: not installed", out)

    def test_bad_pin_files(self):
        for mutate in (
            lambda p: p["image"].update(digest="sha256:" + "0" * 64),
            lambda p: p["image"].update(digest="sha256:abc"),
            lambda p: p["archive"].update(date="2026-10-23"),
            lambda p: p["archive"].update(server="https://archive.archlinux.org/repos/2026/10/22/$repo/os/$arch"),
            lambda p: p.update(packages={}),
            lambda p: p.update(schema="x"),
        ):
            pin = json.loads(json.dumps(PIN))
            mutate(pin)
            with self.subTest(pin=pin):
                self.assertEqual(self.run_verify("clang 23.1.3-4\n", pin)[0], 2)

    def test_packages_lists_names(self):
        with open(self.pin_path, "w", encoding="utf-8") as handle:
            json.dump(PIN, handle)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_toolchain.main(["packages", self.pin_path]), 0)
        self.assertEqual(out.getvalue().split(), ["clang", "vulkan-headers"])

    def test_repository_pin_file_is_well_formed(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        pin, problems = check_toolchain.load_pin(os.path.join(root, "toolchain", "ci-linux.json"))
        self.assertEqual(problems, [])
        for name in ("clang", "gcc", "gcc-libs", "cmake", "ninja", "just", "mold", "ccache", "shellcheck", "python"):
            self.assertIn(name, pin["packages"])


class CheckWorkflowsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        os.makedirs(os.path.join(self.root, ".github", "workflows"))
        with open(os.path.join(self.root, "CMakePresets.json"), "w", encoding="utf-8") as handle:
            json.dump(PRESETS, handle)
        self.write("dependabot.yml", DEPENDABOT, workflow=False)
        self.write("ci.yml", GOOD)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, text, workflow=True):
        sub = os.path.join(".github", "workflows") if workflow else ".github"
        with open(os.path.join(self.root, sub, name), "w", encoding="utf-8") as handle:
            handle.write(text)

    def rules(self, strict=False):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = check_workflows.run(self.root, strict)
        rules = sorted({line.split()[0] for line in out.getvalue().splitlines() if line.startswith("R-CI-")})
        return rc, rules

    def mutate(self, old, new):
        self.write("ci.yml", GOOD.replace(old, new))

    def test_good_file_passes(self):
        self.assertEqual(self.rules(), (0, []))

    def test_unpinned(self):
        self.mutate(f"@{SHA}  # v4.2.2", "@v4  # v4")
        self.assertEqual(self.rules(), (1, ["R-CI-USES-UNPINNED"]))

    def test_missing_version_comment(self):
        self.mutate("  # v4.2.2", "")
        self.assertEqual(self.rules(), (1, ["R-CI-USES-COMMENT"]))

    def test_placeholder_is_warning_then_error_when_strict(self):
        self.mutate(SHA, "0" * 40)
        self.assertEqual(self.rules(), (0, ["R-CI-UNVERIFIED-SHA"]))
        self.assertEqual(self.rules(strict=True), (1, ["R-CI-UNVERIFIED-SHA"]))

    def test_sha256_placeholder(self):
        self.mutate("jobs:", 'env:\n  X_SHA256: "' + "0" * 64 + '"\njobs:')
        self.assertEqual(self.rules(), (0, ["R-CI-UNVERIFIED-SHA"]))

    def test_permissions(self):
        self.mutate("permissions:\n  contents: read\n", "")
        self.assertEqual(self.rules(), (1, ["R-CI-PERMISSIONS"]))
        self.mutate("contents: read", "contents: write")
        self.assertEqual(self.rules(), (1, ["R-CI-PERMISSIONS"]))

    def test_pull_request_target(self):
        self.mutate("  push:\n", "  pull_request_target:\n")
        self.assertEqual(self.rules(), (1, ["R-CI-TRIGGER"]))

    def test_dispatch_required(self):
        self.mutate("  workflow_dispatch:\n", "")
        self.assertEqual(self.rules(), (1, ["R-CI-TRIGGER"]))

    def test_job_and_runner(self):
        self.mutate("ubuntu-24.04", "ubuntu-latest")
        self.assertEqual(self.rules(), (1, ["R-CI-JOB"]))
        self.mutate("  linux:", "  other:")
        self.assertEqual(self.rules(), (1, ["R-CI-JOB"]))

    def test_build_entry(self):
        for command, bad in (
            ("cmake --build build", True),
            ("ninja -C build", True),
            ("just check", True),
            ("cmake --preset nope", True),
            ("cmake --workflow --preset debug", True),
            ("cmake --preset debug", False),
            ("cmake --workflow --preset ci-clang", False),
            ("cmake --version", False),
            ("sudo apt-get install -y ninja-build && ctest", True),
        ):
            with self.subTest(command=command):
                self.mutate("bash scripts/ci-local.sh", command)
                self.assertEqual(self.rules(), (1, ["R-CI-BUILD-ENTRY"]) if bad else (0, []))

    def container_job(self, image=IMAGE, date="2026/10/23", pin=PIN):
        os.makedirs(os.path.join(self.root, "toolchain"), exist_ok=True)
        if pin is not None:
            with open(os.path.join(self.root, "toolchain", "ci-linux.json"), "w", encoding="utf-8") as handle:
                json.dump(pin, handle)
        self.mutate(
            "    runs-on: ubuntu-24.04\n",
            f'    runs-on: ubuntu-24.04\n    container:\n      image: {image}\n    env:\n      ARCHIVE_DATE: "{date}"\n',
        )

    def test_container_pinned_by_digest_passes(self):
        self.container_job()
        self.assertEqual(self.rules(strict=True), (0, []))
        self.mutate("    runs-on: ubuntu-24.04\n", f"    runs-on: ubuntu-24.04\n    container: {IMAGE}\n    env:\n      ARCHIVE_DATE: x\n")
        self.assertEqual(self.rules()[1], ["R-CI-TOOLCHAIN"])

    def test_container_must_be_pinned(self):
        for image in ("archlinux:base-devel", "archlinux@" + DIGEST, "archlinux:base-devel@sha256:abc", "archlinux"):
            with self.subTest(image=image):
                self.container_job(image=image)
                self.assertEqual(self.rules(), (1, ["R-CI-CONTAINER"]))

    def test_container_placeholder_digest(self):
        bad = "ghcr.io/archlinux/archlinux:base-devel@sha256:" + "0" * 64
        self.container_job(image=bad)
        self.assertEqual(self.rules(), (1, ["R-CI-TOOLCHAIN", "R-CI-UNVERIFIED-SHA"]))

    def test_container_must_match_the_pin_file(self):
        self.container_job(image=IMAGE.replace("ab", "cd", 1))
        self.assertEqual(self.rules(), (1, ["R-CI-TOOLCHAIN"]))
        self.container_job(date="2026/10/22")
        self.assertEqual(self.rules(), (1, ["R-CI-TOOLCHAIN"]))
        self.container_job(pin=None)
        os.remove(os.path.join(self.root, "toolchain", "ci-linux.json"))
        self.assertEqual(self.rules(), (1, ["R-CI-TOOLCHAIN"]))

    def test_injection_and_secret(self):
        self.mutate("echo hi", "echo ${{ github.event.pull_request.title }}")
        self.assertEqual(self.rules(), (1, ["R-CI-INJECT"]))
        self.mutate("echo hi", "echo ${{ secrets.TOKEN2 }}")
        self.assertEqual(self.rules(), (1, ["R-CI-SECRET"]))

    def test_parse_error(self):
        self.write("ci.yml", "a: 1\na: 2\n")
        self.assertEqual(self.rules(), (1, ["R-CI-PARSE"]))

    def test_dependabot(self):
        self.write("dependabot.yml", DEPENDABOT.replace("monthly", "weekly"), workflow=False)
        self.assertEqual(self.rules(), (1, ["R-CI-DEPENDABOT"]))

    def test_repository_files_have_no_errors(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_workflows.run(root, False), 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
