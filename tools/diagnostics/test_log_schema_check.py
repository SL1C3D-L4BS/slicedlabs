"""Unit tests for log-schema-check (WO-030): one valid event, each rule planted once."""

import contextlib
import copy
import io
import importlib.util
import json
import os
import shutil
import tempfile
import unittest

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("log_schema_check", os.path.join(_here, "log_schema_check.py"))
lsc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lsc)

SCHEMA = lsc.slib.load_json(lsc.DEFAULT_SCHEMA)
GOOD = {
    "schema": "sl-log-event/1",
    "level": "info",
    "system": "core",
    "message": "build info",
    "fields": {"n": 3, "x": 0.5, "ok": True, "name": "a", "nan": None},
    "correlation": {"id": 0, "scope": "process", "tick": 0},
    "where": {"file": "engine/core/src/version.cpp", "line": 12, "function": "f"},
    "ts": "2026-10-17T12:00:00.000Z",
}


def line(obj):
    return json.dumps(obj, separators=(",", ":"))


def event(**changes):
    obj = copy.deepcopy(GOOD)
    for key, value in changes.items():
        if value is None:
            del obj[key]
        else:
            obj[key] = value
    return obj


class CheckLinesTest(unittest.TestCase):
    def check(self, lines, require_levels=False):
        findings, _, _ = lsc.check_lines(lines, SCHEMA, "t.jsonl", require_levels)
        return findings

    def test_valid_event_passes(self):
        self.assertEqual(self.check([line(GOOD)]), [])

    def test_schema_is_inside_the_subset(self):
        self.assertEqual(lsc.slib.check_schema_subset(SCHEMA), [])

    def test_non_string_system_names_the_field(self):
        findings = self.check([line(GOOD), line(event(system=42))])
        self.assertEqual(len(findings), 1)
        self.assertIn("t.jsonl:2: R-LOG-SCHEMA /system: expected type string", findings[0])

    def test_missing_correlation_is_reported(self):
        findings = self.check([line(event(correlation=None))])
        self.assertTrue(any("missing required property 'correlation'" in f for f in findings), findings)

    def test_correlation_fields_are_required(self):
        findings = self.check([line(event(correlation={"id": 1, "tick": 0}))])
        self.assertTrue(any("/correlation" in f and "'scope'" in f for f in findings), findings)

    def test_bad_level_timestamp_and_field_type(self):
        findings = self.check([line(event(level="verbose", ts="2026-10-17 12:00", fields={"a": [1]}))])
        text = "\n".join(findings)
        self.assertIn("/level", text)
        self.assertIn("/ts", text)
        self.assertIn("/fields/a", text)

    def test_invalid_json_duplicate_keys_and_nan(self):
        findings = self.check(["{not json", '{"a":1,"a":2}', '{"a":NaN}', ""])
        self.assertEqual([f.split(" ")[1] for f in findings], ["R-LOG-JSON"] * 4 + ["R-LOG-EMPTY"])
        self.assertIn("duplicate object key", findings[1])

    def test_empty_file(self):
        self.assertEqual(self.check([]), ["t.jsonl: R-LOG-EMPTY no log event in the file"])

    def test_level_coverage(self):
        lines = [line(event(level=lv)) for lv in lsc.LEVELS]
        self.assertEqual(self.check(lines, require_levels=True), [])
        findings = self.check(lines[:-1], require_levels=True)
        self.assertEqual(len(findings), 1)
        self.assertIn("R-LOG-COVERAGE level(s) never logged: fatal", findings[0])


class MainTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="log-schema-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def run_main(self, text):
        path = os.path.join(self.tmp, "events.jsonl")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return self.quiet_main(["--file", path])

    def quiet_main(self, argv):
        with contextlib.redirect_stdout(io.StringIO()):
            return lsc.main(argv)

    def test_exit_codes(self):
        self.assertEqual(self.run_main(line(GOOD) + "\n"), 0)
        self.assertEqual(self.run_main(line(event(system=1)) + "\n"), 1)
        self.assertEqual(self.quiet_main(["--file", os.path.join(self.tmp, "absent.jsonl")]), 1)


if __name__ == "__main__":
    unittest.main()
