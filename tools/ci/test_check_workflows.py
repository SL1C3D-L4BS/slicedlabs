"""Unit tests for tools/ci/check_workflows.py and tools/ci/miniyaml.py (WO-028)."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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
