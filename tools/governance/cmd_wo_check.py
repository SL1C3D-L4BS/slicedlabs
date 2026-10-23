"""wo-check: every work-orders/WO-*.md follows the factory work-order template (~/.dotfiles WO.md, wolib.py).

Rules (each failure line starts with the rule id):
  R-WO-FRONT     front matter parses (the WO.md YAML subset), has exactly the template keys with sane values,
                 and `id` matches the file name WO-NNN.md
  R-WO-CLASS     class is S, M or L
  R-WO-BUDGET    budget is {tokens, usd, rounds}; within the class cap (S 300000/3, M 1000000/8, L 1500000/20);
                 rounds are 0 to 2
  R-WO-ACCEPT    acceptance is a non-empty list of {cmd, expect_exit}; every cmd is `just <recipe> [args]` and
                 the recipe exists in the justfile
  R-WO-TRACES    the body has one `Traces: SL-EP-nnn ...` line of well-formed, unique ids
  R-WO-REGISTRY  every traced id exists in registry/SL-REG-002-episodes.json (skipped, with a note, while the
                 registry file does not exist yet)

Stdlib only (ADR-0004); run with `python3 -I cmd_wo_check.py [--root DIR]`.
"""

import glob
import json
import os
import re
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

WO_KEYS = (
    "id",
    "class",
    "repo",
    "risk",
    "goal",
    "non_goals",
    "acceptance",
    "paths_allowed",
    "touches_gates",
    "budget",
    "approver",
)
RISKS = ("none", "gpu", "governed", "deps", "ci", "security", "data", "deploy")
CLASS_CAPS = {"S": (300000, 3), "M": (1000000, 8), "L": (1500000, 20)}
MAX_ROUNDS = 2
APPROVERS = ("auto", "founder", "fable")
REPO = "slicedlabs"
REGISTRY = "registry/SL-REG-002-episodes.json"

FILE_RE = re.compile(r"^WO-(\d{3})\.md$")
EP_RE = re.compile(r"^SL-EP-\d{3}$")
TRACES_RE = re.compile(r"^Traces:(.*)$")
ARG_RE = re.compile(r"^(?:-[A-Za-z]|[A-Za-z0-9_./:=+@%,][A-Za-z0-9_./:=+@%,-]*)$")
KEY_RE = re.compile(r"^([A-Za-z_][\w-]*)\s*:(?:\s+(.*))?$")


class WoParseError(ValueError):
    pass


# --- the WO.md YAML subset: maps, block lists (of scalars or maps), flow lists, scalars -------------


def _value(raw):
    v = raw.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        return v[1:-1]
    if v.startswith("["):
        if not v.endswith("]"):
            raise WoParseError("unterminated flow list")
        return list(slgov._split_flow(v[1:-1]))
    if v in ("true", "false"):
        return v == "true"
    if v in ("null", "~"):
        return None
    if re.fullmatch(r"-?\d+", v):
        return int(v)
    if re.fullmatch(r"-?\d+\.\d+", v):
        return float(v)
    return v


def _parse_map(entries, i, indent):
    out = {}
    while i < len(entries) and entries[i][0] == indent and not entries[i][1].startswith("- "):
        _, content, n = entries[i]
        m = KEY_RE.match(content)
        if not m:
            raise WoParseError("line %d: expected 'key: value'" % n)
        key, raw = m.group(1), m.group(2)
        if key in out:
            raise WoParseError("line %d: duplicate key '%s'" % (n, key))
        i += 1
        if raw:
            out[key] = _value(raw)
        elif i < len(entries) and entries[i][0] > indent:
            out[key], i = _parse_block(entries, i, entries[i][0])
        else:
            out[key] = ""
    return out, i


def _parse_list(entries, i, indent):
    out = []
    while i < len(entries) and entries[i][0] == indent and (entries[i][1].startswith("- ") or entries[i][1] == "-"):
        _, content, n = entries[i]
        rest = content[1:].lstrip(" ")
        if not rest:
            raise WoParseError("line %d: empty list item" % n)
        offset = len(content) - len(rest)
        if KEY_RE.match(rest) and not rest.startswith(("\"", "'", "[")):
            entries[i] = (indent + offset, rest, n)
            item, i = _parse_map(entries, i, indent + offset)
            out.append(item)
        else:
            out.append(_value(rest))
            i += 1
    return out, i


def _parse_block(entries, i, indent):
    if entries[i][1].startswith("- ") or entries[i][1] == "-":
        return _parse_list(entries, i, indent)
    return _parse_map(entries, i, indent)


def parse_work_order(text):
    """Return (front matter dict, body text). Raises WoParseError (or slgov.FrontMatterError)."""
    lines, body = slgov.split_front_matter(text)
    entries = []
    for n, line in enumerate(lines, start=2):
        if "\t" in line:
            raise WoParseError("line %d: tabs are not allowed" % n)
        content = slgov._strip_comment(line).rstrip()
        if content.strip():
            entries.append((len(content) - len(content.lstrip(" ")), content.strip(), n))
    if not entries:
        raise WoParseError("front matter is empty")
    data, i = _parse_map(entries, 0, entries[0][0])
    if i < len(entries):
        raise WoParseError("line %d: unexpected indentation or syntax" % entries[i][2])
    return data, body


# --- checks --------------------------------------------------------------------------------------


def _num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def check_front(meta, filename):
    errors = []
    for key in WO_KEYS:
        if key not in meta:
            errors.append(("R-WO-FRONT", "front matter lacks '%s'" % key))
    for key in sorted(set(meta) - set(WO_KEYS)):
        errors.append(("R-WO-FRONT", "unknown front matter key '%s'" % key))
    m = FILE_RE.match(filename)
    if not m:
        errors.append(("R-WO-FRONT", "file name must be WO-NNN.md"))
    elif "id" in meta and meta["id"] != "WO-" + m.group(1):
        errors.append(("R-WO-FRONT", "id %r does not match the file name %s" % (meta["id"], filename)))
    if "repo" in meta and meta["repo"] != REPO:
        errors.append(("R-WO-FRONT", "repo must be %s" % REPO))
    if "goal" in meta and not (isinstance(meta["goal"], str) and meta["goal"].strip()):
        errors.append(("R-WO-FRONT", "goal must be a non-empty string"))
    elif "goal" in meta and ("\n" in meta["goal"] or len(meta["goal"]) > 240):
        errors.append(("R-WO-FRONT", "goal must be one sentence of 240 characters or fewer (wo-lint rule)"))
    risk = meta.get("risk")
    if "risk" in meta:
        if not isinstance(risk, list) or not risk or any(r not in RISKS for r in risk):
            errors.append(("R-WO-FRONT", "risk must be a non-empty list from %s" % ", ".join(RISKS)))
        elif "none" in risk and len(risk) > 1:
            errors.append(("R-WO-FRONT", "risk 'none' cannot be combined with other flags"))
    for key in ("non_goals", "paths_allowed"):
        v = meta.get(key)
        if key in meta and (not isinstance(v, list) or not v or any(not isinstance(x, str) or not x for x in v)):
            errors.append(("R-WO-FRONT", "%s must be a non-empty list of strings" % key))
    if "touches_gates" in meta and not isinstance(meta["touches_gates"], bool):
        errors.append(("R-WO-FRONT", "touches_gates must be true or false"))
    ap = meta.get("approver")
    if "approver" in meta:
        if ap not in APPROVERS:
            errors.append(("R-WO-FRONT", "approver must be auto, founder or fable"))
        elif ap == "auto" and (meta.get("class") == "L" or (isinstance(risk, list) and risk != ["none"])):
            errors.append(("R-WO-FRONT", "approver auto is not allowed for class L or any risk flag"))
    return errors


def check_class_budget(meta):
    errors = []
    cls = meta.get("class")
    if "class" in meta and cls not in CLASS_CAPS:
        errors.append(("R-WO-CLASS", "class must be S, M or L, found %r" % (cls,)))
    b = meta.get("budget")
    if "budget" not in meta:
        return errors
    if not isinstance(b, dict) or set(b) != {"tokens", "usd", "rounds"}:
        errors.append(("R-WO-BUDGET", "budget must be a map with exactly tokens, usd, rounds"))
        return errors
    t, u, r = b["tokens"], b["usd"], b["rounds"]
    if not (_num(t) and t > 0 and float(t).is_integer()):
        errors.append(("R-WO-BUDGET", "budget.tokens must be a positive integer"))
    if not (_num(u) and u > 0):
        errors.append(("R-WO-BUDGET", "budget.usd must be a positive number"))
    if isinstance(r, bool) or not isinstance(r, int) or not 0 <= r <= MAX_ROUNDS:
        errors.append(("R-WO-BUDGET", "budget.rounds must be an integer from 0 to %d" % MAX_ROUNDS))
    if cls in CLASS_CAPS and _num(t) and _num(u):
        cap_t, cap_u = CLASS_CAPS[cls]
        if t > cap_t or u > cap_u:
            errors.append(("R-WO-BUDGET", "budget above the class %s cap (%d tokens, $%d)" % (cls, cap_t, cap_u)))
    return errors


def acceptance_cmd_error(cmd, recipes):
    """None when cmd is `just <recipe> [args]` for a recipe in the justfile, else a message."""
    if not isinstance(cmd, str) or not cmd.strip():
        return "cmd must be a non-empty string"
    try:
        argv = shlex.split(cmd)
    except ValueError as exc:
        return "cmd %r does not parse: %s" % (cmd, exc)
    if len(argv) < 2 or argv[0] != "just":
        return "cmd %r must be `just <recipe> [args]`" % cmd
    if any(not ARG_RE.match(a) for a in argv[1:]) or cmd != " ".join(argv):
        return "cmd %r contains shell syntax" % cmd
    if argv[1] not in recipes:
        return "cmd %r names recipe '%s' which is not in the justfile" % (cmd, argv[1])
    return None


def check_acceptance(meta, recipes):
    if "acceptance" not in meta:
        return []
    acc = meta["acceptance"]
    if not isinstance(acc, list) or not acc:
        return [("R-WO-ACCEPT", "acceptance must be a non-empty list of {cmd, expect_exit}")]
    errors = []
    for k, item in enumerate(acc, start=1):
        if not isinstance(item, dict) or set(item) != {"cmd", "expect_exit"}:
            errors.append(("R-WO-ACCEPT", "acceptance item %d must have exactly cmd and expect_exit" % k))
            continue
        msg = acceptance_cmd_error(item["cmd"], recipes)
        if msg:
            errors.append(("R-WO-ACCEPT", "acceptance item %d: %s" % (k, msg)))
        ee = item["expect_exit"]
        if isinstance(ee, bool) or not isinstance(ee, int) or ee < 0:
            errors.append(("R-WO-ACCEPT", "acceptance item %d: expect_exit must be a non-negative integer" % k))
    return errors


def traced_ids(body):
    """(ids, errors) from the `Traces:` line of a work-order body."""
    lines = [m for m in (TRACES_RE.match(line) for line in body.splitlines()) if m]
    if not lines:
        return [], [("R-WO-TRACES", "missing 'Traces: SL-EP-nnn' line in the body")]
    if len(lines) > 1:
        return [], [("R-WO-TRACES", "more than one 'Traces:' line")]
    ids = lines[0].group(1).split()
    if not ids:
        return [], [("R-WO-TRACES", "'Traces:' line lists no episode ids")]
    errors = []
    for tid in ids:
        if not EP_RE.match(tid):
            errors.append(("R-WO-TRACES", "'%s' is not a well-formed SL-EP-nnn id" % tid))
    if len(set(ids)) != len(ids):
        errors.append(("R-WO-TRACES", "duplicate id in the 'Traces:' line"))
    return ids, errors


def check_work_order(text, filename, recipes, episode_ids=None):
    """All errors for one work order as (rule, message). episode_ids None skips the registry check."""
    try:
        meta, body = parse_work_order(text)
    except (WoParseError, slgov.FrontMatterError) as exc:
        return [("R-WO-FRONT", "front matter does not parse: %s" % exc)]
    errors = check_front(meta, filename) + check_class_budget(meta) + check_acceptance(meta, recipes)
    ids, trace_errors = traced_ids(body)
    errors += trace_errors
    if episode_ids is not None:
        for tid in ids:
            if EP_RE.match(tid) and tid not in episode_ids:
                errors.append(("R-WO-REGISTRY", "%s is not in %s" % (tid, REGISTRY)))
    return errors


def load_episode_ids(root):
    """(set of ids or None, note or None, error or None)."""
    path = os.path.join(root, REGISTRY)
    if not os.path.isfile(path):
        return None, "wo-check: note: %s does not exist yet, registry cross-check skipped" % REGISTRY, None
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        return {e["id"] for e in data["episodes"]}, None, None
    except (OSError, ValueError, KeyError, TypeError) as exc:
        return None, None, "%s:1: R-WO-REGISTRY: cannot read the episode ids: %s" % (REGISTRY, exc)


def run(root):
    with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
        recipes = slgov.parse_justfile(fh.read())
    ids, note, reg_error = load_episode_ids(root)
    paths = sorted(glob.glob(os.path.join(root, "work-orders", "WO-*.md")))
    problems = [reg_error] if reg_error else []
    for path in paths:
        name = os.path.basename(path)
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        for rule, message in check_work_order(text, name, recipes, ids):
            problems.append("work-orders/%s:1: %s: %s" % (name, rule, message))
    if not paths:
        problems.append("work-orders/:1: R-WO-FRONT: no work orders found")
    if note:
        print(note)
    for line in problems:
        print(line)
    if problems:
        print("wo-check: %d problem(s)" % len(problems))
        return 1
    print("wo-check: ok (%d work orders)" % len(paths))
    return 0


def main(argv):
    root = slgov.ROOT
    args = argv[1:]
    if args[:1] == ["--root"] and len(args) == 2:
        root = os.path.abspath(args[1])
    elif args:
        print("usage: cmd_wo_check.py [--root DIR]", file=sys.stderr)
        return 2
    return run(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
