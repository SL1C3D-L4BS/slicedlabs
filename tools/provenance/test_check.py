"""Unit tests for provenance-check (WO-029): one synthetic record, each rule planted once."""

import copy
import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
import unittest

# loaded by path under a unique name: tools/format also has a check.py
_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("provenance_check", os.path.join(_here, "check.py"))
check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="prov-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "repo")
        self.build = os.path.join(self.root, "build", "debug")
        os.makedirs(os.path.join(self.root, "src"))
        os.makedirs(os.path.join(self.build, "provenance"))
        shutil.copytree(os.path.join(check.ROOT, "schemas"), os.path.join(self.root, "schemas"))
        self.write("src/a.cpp", b"int a;\n")
        self.write("build/debug/libx.a", b"artifact")
        record = {
            "schema": "sl-provenance/1",
            "target": "x",
            "artifact": {"path": "libx.a", "sha256": sha(b"artifact")},
            "inputs": [{"path": "src/a.cpp", "sha256": sha(b"int a;\n")}],
            "toolchain": {
                "compiler": "Clang",
                "compiler_version": "1",
                "cmake": "4",
                "linker": "mold",
                "lock_sha256": "x",
            },
            "parameters": {
                "preset": "debug",
                "build_type": "Debug",
                "std": 23,
                "sanitize": "none",
                "warnings_as_errors": False,
                "flags_sha256": sha(b"f"),
            },
            "built_at": "2026-10-17T00:00:00Z",
        }
        record["environment_hash"] = check.environment_hash(record)
        self.record = record
        self.save(record)

    def write(self, rel, data):
        path = os.path.join(self.root, rel)
        with open(path, "wb") as fh:
            fh.write(data)

    def save(self, record):
        with open(os.path.join(self.build, "provenance", "x.prov.json"), "w", encoding="utf-8") as fh:
            json.dump(record, fh)

    def rules(self):
        return sorted({f[0] for f in check.run(self.root, self.build, ["x"])[1]})

    def test_clean(self):
        self.assertEqual(self.rules(), [])

    def test_stale_input_names_the_file(self):
        self.write("src/a.cpp", b"int b;\n")
        names, findings = check.run(self.root, self.build, ["x"])
        self.assertEqual([f[0] for f in findings], ["R-PROV-INPUT-STALE"])
        self.assertIn("src/a.cpp", findings[0][2])

    def test_missing_input(self):
        os.remove(os.path.join(self.root, "src", "a.cpp"))
        self.assertEqual(self.rules(), ["R-PROV-INPUT-MISSING"])

    def test_artifact_changed_and_missing(self):
        self.write("build/debug/libx.a", b"other")
        self.assertEqual(self.rules(), ["R-PROV-ARTIFACT-SHA"])
        os.remove(os.path.join(self.build, "libx.a"))
        self.assertEqual(self.rules(), ["R-PROV-ARTIFACT-MISSING"])

    def test_env_hash(self):
        record = copy.deepcopy(self.record)
        record["parameters"]["std"] = 26
        self.save(record)
        self.assertEqual(self.rules(), ["R-PROV-ENV-HASH"])

    def test_schema_and_load(self):
        record = copy.deepcopy(self.record)
        del record["built_at"]
        self.save(record)
        self.assertEqual(self.rules(), ["R-PROV-SCHEMA"])
        os.remove(os.path.join(self.build, "provenance", "x.prov.json"))
        self.assertEqual(self.rules(), ["R-PROV-LOAD"])

    def test_input_path_escape(self):
        record = copy.deepcopy(self.record)
        record["inputs"][0]["path"] = "../outside"
        self.save(record)
        self.assertEqual(self.rules(), ["R-PROV-INPUT-PATH"])


if __name__ == "__main__":
    unittest.main()
