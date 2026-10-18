#!/usr/bin/env python3
"""fabric-event-schema-check (WO-047, SL-EP-069, SL-ARCH-001 ARCH1-R46 to ARCH1-R49, SL-GOV-006 F6): every line of the
Fabric event JSON Lines dump is a complete envelope.

  python3 -I tools/fabric/event_schema_check.py [--file PATH] [--schema PATH]

Reads the file the sl::fabric::event_bus JSON sink wrote (default build/fabric/events.jsonl; `just
fabric-event-schema-check` produces it by running the CTest label fabric_events with SL_FABRIC_EVENT_JSON_PATH) and
validates each line against schemas/sl-fabric-event.schema.json with the stdlib subset validator
(tools/governance/slib.py). The subset has no oneOf, so "exactly one of subject id or scope tag" is checked here.
Findings, one per line as `<file>:<line>: <rule> <json pointer>: <message>`:
  R-EVT-LOAD         the file or the schema is missing, unreadable, not UTF-8 or outside the validator subset
  R-EVT-JSON         a line is blank, is not JSON, or repeats a key
  R-EVT-CORRELATION  the correlation object or one of id, scope, tick is missing or malformed (F6)
  R-EVT-ABOUT        a line has both or neither of subject and scope
  R-EVT-SCHEMA       any other violation of the schema
  R-EVT-EMPTY        the file holds no event
Exit 0 when clean, 1 with the findings. Stdlib only, reads files, runs nothing (ADR-0004).
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import slib  # noqa: E402

DEFAULT_FILE = os.path.join("build", "fabric", "events.jsonl")
DEFAULT_SCHEMA = os.path.join(ROOT, "schemas", "sl-fabric-event.schema.json")
MISSING = re.compile(r"^missing required property '(.*)'$")


def _unique_keys(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError("duplicate object key %r" % key)
        obj[key] = value
    return obj


def _reject_constant(name):
    raise ValueError("%s is not JSON" % name)


def parse_line(line):
    """Strict JSON: no duplicate keys, no NaN or Infinity. Raises ValueError."""
    return json.loads(line, object_pairs_hook=_unique_keys, parse_constant=_reject_constant)


def _located(pointer, message):
    """A missing required property is reported at the property itself (/correlation/scope)."""
    found = MISSING.match(message)
    if found:
        pointer = (pointer.rstrip("/") or "") + "/" + found.group(1)
    return pointer, message


def check_object(obj, schema):
    """Findings (rule, pointer, message) of one parsed line."""
    findings = []
    for pointer, message in slib.validate(obj, schema):
        pointer, message = _located(pointer, message)
        rule = "R-EVT-CORRELATION" if pointer.startswith("/correlation") else "R-EVT-SCHEMA"
        findings.append((rule, pointer, message))
    if isinstance(obj, dict):
        has_subject, has_scope = "subject" in obj, "scope" in obj
        if has_subject == has_scope:
            what = "both subject and scope" if has_subject else "neither subject nor scope"
            findings.append(("R-EVT-ABOUT", "/", "exactly one of subject id or scope tag is required, found " + what))
    return findings


def check_lines(lines, schema, origin="<lines>"):
    """Validate JSON Lines text (a list of lines without newlines); return (findings, event count)."""
    findings = []
    events = 0
    for number, line in enumerate(lines, 1):
        where = "%s:%d" % (origin, number)
        if not line.strip():
            findings.append("%s: R-EVT-JSON blank line (JSON Lines allows none)" % where)
            continue
        try:
            obj = parse_line(line)
        except ValueError as exc:
            findings.append("%s: R-EVT-JSON %s" % (where, exc))
            continue
        events += 1
        for rule, pointer, message in check_object(obj, schema):
            findings.append("%s: %s %s: %s" % (where, rule, pointer, message))
    if events == 0 and not findings:
        findings.append("%s: R-EVT-EMPTY no event in the file" % origin)
    elif events == 0:
        findings.append("%s: R-EVT-EMPTY no valid JSON event in the file" % origin)
    return findings, events


def read_lines(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="fabric-event-schema-check", description=__doc__.splitlines()[0])
    ap.add_argument("--file", default=DEFAULT_FILE, help="JSON Lines dump (default: %(default)s)")
    ap.add_argument("--schema", default=DEFAULT_SCHEMA, help="schema file (default: schemas/sl-fabric-event)")
    args = ap.parse_args(argv)

    try:
        schema = slib.load_json(args.schema)
    except slib.LoadError as exc:
        print("fabric-event-schema-check: R-EVT-LOAD %s" % exc)
        return 1
    subset = slib.check_schema_subset(schema)
    if subset:
        print("fabric-event-schema-check: R-EVT-LOAD %s is outside the validator subset: %s" % (args.schema, subset[0]))
        return 1
    try:
        lines = read_lines(args.file)
    except (OSError, UnicodeDecodeError) as exc:
        print("fabric-event-schema-check: R-EVT-LOAD cannot read %s: %s" % (args.file, exc))
        return 1

    findings, events = check_lines(lines, schema, args.file)
    for finding in findings:
        print(finding)
    if findings:
        print("fabric-event-schema-check: FAIL (%d finding(s) in %d line(s) of %s)"
              % (len(findings), len(lines), args.file))
        return 1
    print("fabric-event-schema-check: ok (%d events valid against sl-fabric-event)" % events)
    return 0


if __name__ == "__main__":
    sys.exit(main())
