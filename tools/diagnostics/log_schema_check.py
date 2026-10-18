#!/usr/bin/env python3
"""log-schema-check (WO-030, SL-EP-056, SL-GOV-006 F6): every structured log line validates against its schema.

  python3 -I tools/diagnostics/log_schema_check.py [--file PATH] [--schema PATH] [--require-levels]

Reads a JSON Lines file written by sl::log::json_sink (default: build/debug/log-events.jsonl; `just log-schema-check`
produces it by running sl_tests with SL_LOG_JSON_PATH) and validates every line against
schemas/sl-log-event.schema.json with the stdlib subset validator (tools/governance/slib.py). Findings:
  R-LOG-LOAD      the file or the schema is missing, unreadable or not UTF-8
  R-LOG-JSON      a line is blank, is not JSON, or repeats a key
  R-LOG-SCHEMA    a line does not validate; the finding names the JSON pointer (for example /system)
  R-LOG-EMPTY     the file holds no event
  R-LOG-COVERAGE  with --require-levels: one of the six levels never appears
Exit 0 when clean, 1 with one line per finding. Stdlib only; reads files, runs nothing (ADR-0004).
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "governance"))
import slib  # noqa: E402

DEFAULT_FILE = os.path.join("build", "debug", "log-events.jsonl")
DEFAULT_SCHEMA = os.path.join(ROOT, "schemas", "sl-log-event.schema.json")
LEVELS = ("trace", "debug", "info", "warn", "error", "fatal")


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


def check_lines(lines, schema, origin="<lines>", require_levels=False):
    """Validate JSON Lines text (a list of lines without newlines); return (findings, events, levels seen)."""
    findings = []
    events = 0
    seen = set()
    for number, line in enumerate(lines, 1):
        where = "%s:%d" % (origin, number)
        if not line.strip():
            findings.append("%s: R-LOG-JSON blank line (JSON Lines allows none)" % where)
            continue
        try:
            obj = parse_line(line)
        except ValueError as exc:
            findings.append("%s: R-LOG-JSON %s" % (where, exc))
            continue
        events += 1
        for pointer, message in slib.validate(obj, schema):
            findings.append("%s: R-LOG-SCHEMA %s: %s" % (where, pointer, message))
        if isinstance(obj, dict) and obj.get("level") in LEVELS:
            seen.add(obj["level"])
    if events == 0:
        findings.append("%s: R-LOG-EMPTY no log event in the file" % origin)
    elif require_levels:
        missing = [lv for lv in LEVELS if lv not in seen]
        if missing:
            findings.append("%s: R-LOG-COVERAGE level(s) never logged: %s" % (origin, ", ".join(missing)))
    return findings, events, seen


def read_lines(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="log-schema-check", description=__doc__.splitlines()[0])
    ap.add_argument("--file", default=DEFAULT_FILE, help="JSON Lines file (default: %(default)s)")
    ap.add_argument("--schema", default=DEFAULT_SCHEMA, help="schema file (default: schemas/sl-log-event)")
    ap.add_argument("--require-levels", action="store_true", help="every level must appear at least once")
    args = ap.parse_args(argv)

    try:
        schema = slib.load_json(args.schema)
    except slib.LoadError as exc:
        print("log-schema-check: R-LOG-LOAD %s" % exc)
        return 1
    subset = slib.check_schema_subset(schema)
    if subset:
        print("log-schema-check: R-LOG-LOAD %s is outside the validator subset: %s" % (args.schema, subset[0]))
        return 1
    try:
        lines = read_lines(args.file)
    except (OSError, UnicodeDecodeError) as exc:
        print("log-schema-check: R-LOG-LOAD cannot read %s: %s" % (args.file, exc))
        return 1

    findings, events, seen = check_lines(lines, schema, args.file, args.require_levels)
    for f in findings:
        print(f)
    if findings:
        print("log-schema-check: FAIL (%d finding(s) in %d line(s) of %s)" % (len(findings), len(lines), args.file))
        return 1
    levels = ",".join(lv for lv in LEVELS if lv in seen)
    print("log-schema-check: ok (%d events valid against %s; levels %s)" % (events, "sl-log-event", levels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
