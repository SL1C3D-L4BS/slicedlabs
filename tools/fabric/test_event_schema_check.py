"""Tests of the Fabric event schema check (WO-047). FAB-INV-EVT-05: a dump of valid envelopes validates against
schemas/sl-fabric-event.schema.json; every planted violation fails naming its rule, the JSON pointer and the line.
"""

import contextlib
import copy
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_schema_check as esc  # noqa: E402

SCHEMA = esc.slib.load_json(esc.DEFAULT_SCHEMA)
ID_TAIL = "0C0000000000000000000001"
GOOD = {
    "format": "sl-fabric-event/1",
    "id": "evt_090A02JWSJBNWYYT5ZCHMV2MKZ",
    "schema": {"type": "typ_03" + ID_TAIL, "major": 1, "minor": 0},
    "type": "fabric.tx.committed",
    "time_us": 1700000000000000,
    "seq": 7,
    "actor": "act_0A" + ID_TAIL,
    "subject": "ent_05" + ID_TAIL,
    "correlation": {"id": 11, "scope": "tx-manager", "tick": 3},
    "causality": {
        "cause": "evt_090885AG57MZ0RE3GQ2K7J8V73",
        "tx": "txn_080C0000000000000000000005",
        "revision": "sha256:" + "ab" * 32,
    },
    "payload": "0a2a",
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


def check(lines):
    findings, _ = esc.check_lines(lines, SCHEMA, "t.jsonl")
    return findings


class DumpTest(unittest.TestCase):
    def test_fab_inv_evt_05_dump_valid(self):
        scoped = event(subject=None, scope="workspace", causality={}, payload=None)
        bare = event(causality={"tx": "txn_080C0000000000000000000005"})
        lines = [line(GOOD), line(scoped), line(bare)]
        findings, events = esc.check_lines(lines, SCHEMA, "t.jsonl")
        self.assertEqual(findings, [])
        self.assertEqual(events, 3)

    def test_the_committed_schema_is_inside_the_validator_subset(self):
        self.assertEqual(esc.slib.check_schema_subset(SCHEMA), [])

    def test_missing_correlation_scope_names_the_pointer_the_rule_and_the_line(self):
        planted = event(correlation={"id": 11, "tick": 3})
        findings = check([line(GOOD), line(planted)])
        self.assertEqual(len(findings), 1)
        self.assertIn("t.jsonl:2:", findings[0])
        self.assertIn("R-EVT-CORRELATION /correlation/scope", findings[0])

    def test_each_missing_correlation_member_is_named(self):
        for member in ("id", "scope", "tick"):
            corr = {"id": 1, "scope": "s", "tick": 0}
            del corr[member]
            findings = check([line(event(correlation=corr))])
            self.assertEqual(len(findings), 1, member)
            self.assertIn("R-EVT-CORRELATION /correlation/" + member, findings[0])

    def test_a_missing_correlation_object_is_named(self):
        findings = check([line(event(correlation=None))])
        self.assertEqual(len(findings), 1)
        self.assertIn("R-EVT-CORRELATION /correlation", findings[0])

    def test_an_empty_correlation_scope_fails(self):
        findings = check([line(event(correlation={"id": 0, "scope": "", "tick": 0}))])
        self.assertTrue(any("R-EVT-CORRELATION /correlation/scope" in f for f in findings))

    def test_exactly_one_of_subject_and_scope(self):
        both = check([line(event(scope="workspace"))])
        self.assertEqual(len(both), 1)
        self.assertIn("R-EVT-ABOUT", both[0])
        self.assertIn("both subject and scope", both[0])
        neither = check([line(event(subject=None))])
        self.assertEqual(len(neither), 1)
        self.assertIn("neither subject nor scope", neither[0])

    def test_other_envelope_violations_name_the_pointer(self):
        cases = {
            "/id": event(id="evt_not-an-id"),
            "/actor": event(actor="ent_05" + ID_TAIL),
            "/seq": event(seq=0),
            "/type": event(type="has space"),
            "/schema/major": event(schema={"type": "typ_03" + ID_TAIL, "major": -1, "minor": 0}),
            "/causality/revision": event(causality={"revision": "md5:00"}),
            "/payload": event(payload="xyz"),
            "/time_us": event(time_us="now"),
            "/format": event(format="sl-fabric-event/2"),
        }
        for pointer, obj in cases.items():
            findings = check([line(obj)])
            self.assertTrue(any("R-EVT-SCHEMA " + pointer + ":" in f for f in findings), (pointer, findings))

    def test_a_missing_required_member_is_named(self):
        for member in ("format", "id", "schema", "type", "time_us", "seq", "actor", "causality"):
            findings = check([line(event(**{member: None}))])
            self.assertTrue(any("R-EVT-SCHEMA /" + member + ":" in f for f in findings), (member, findings))

    def test_an_unknown_member_is_refused(self):
        findings = check([line(event(extra=1))])
        self.assertTrue(any("R-EVT-SCHEMA /extra" in f for f in findings))

    def test_json_problems(self):
        findings = check(["", "{not json", '{"a":1,"a":2}', "[NaN]"])
        self.assertEqual(len(findings) - 1, 4)
        self.assertTrue(all("R-EVT-JSON" in f for f in findings[:4]))
        self.assertIn("R-EVT-EMPTY", findings[4])

    def test_an_empty_file_fails(self):
        findings = check([])
        self.assertEqual(len(findings), 1)
        self.assertIn("R-EVT-EMPTY", findings[0])


class CommandTest(unittest.TestCase):
    def run_main(self, lines, extra=()):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "events.jsonl")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("".join(item + "\n" for item in lines))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = esc.main(["--file", path] + list(extra))
            return status, out.getvalue()

    def test_clean_file_exits_zero(self):
        status, out = self.run_main([line(GOOD)])
        self.assertEqual(status, 0)
        self.assertIn("ok (1 events", out)

    def test_planted_file_exits_one_naming_pointer_and_line(self):
        status, out = self.run_main([line(GOOD), line(event(correlation={"id": 1, "tick": 0}))])
        self.assertEqual(status, 1)
        self.assertIn(":2: R-EVT-CORRELATION /correlation/scope", out)
        self.assertIn("FAIL", out)

    def test_missing_file_is_a_load_finding(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = esc.main(["--file", os.path.join(tempfile.gettempdir(), "no-such-events.jsonl")])
        self.assertEqual(status, 1)
        self.assertIn("R-EVT-LOAD", out.getvalue())


if __name__ == "__main__":
    unittest.main()
