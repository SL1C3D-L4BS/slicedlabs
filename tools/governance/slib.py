"""Registry library (WO-002): JSON loading, a JSON Schema subset validator, the episode lifecycle
FSM (master plan 6.2) and the registry rule engine.

Stdlib only, no subprocess, no network (ADR-0004). Run the command scripts with `python3 -I`.

Findings are dicts {"rule": "R-...", "path": "/episodes/3/status", "message": "..."}; an empty list
means the registry is sound. The rule engine is pure: callers pass the registry and the context
(documents, work-order traces, plan sha256, view hashes) as already-parsed data.
"""

import hashlib
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCHEMA_DIR = os.path.join(ROOT, "schemas")


class LoadError(ValueError):
    """A JSON file is missing, unreadable, malformed or has duplicate object keys."""


class SchemaError(ValueError):
    """A schema uses a keyword outside the supported subset, or is otherwise malformed."""


# --- JSON loading -----------------------------------------------------------------------------


def _no_duplicate_keys(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError("duplicate object key %r" % key)
        seen[key] = value
    return seen


def loads_json(text, origin="<string>"):
    try:
        return json.loads(text, object_pairs_hook=_no_duplicate_keys)
    except ValueError as exc:
        raise LoadError("%s: %s" % (origin, exc)) from exc


def load_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return loads_json(fh.read(), path)
    except OSError as exc:
        raise LoadError("%s: %s" % (path, exc.strerror or exc)) from exc


_SCHEMA_CACHE = {}


def load_schema(name, schema_dir=None):
    """Load schemas/<name>.schema.json (name like 'sl-reg-002'); cached per directory."""
    directory = schema_dir or SCHEMA_DIR
    key = (directory, name)
    if key not in _SCHEMA_CACHE:
        _SCHEMA_CACHE[key] = load_json(os.path.join(directory, name + ".schema.json"))
    return _SCHEMA_CACHE[key]


# --- JSON Schema subset validator -------------------------------------------------------------

# Keywords that only annotate; they never reject an instance.
ANNOTATION_KEYWORDS = frozenset(
    ["$schema", "$id", "$comment", "$defs", "title", "description", "default", "examples"]
)
# Keywords that validate. Anything else in a schema is refused (SchemaError), never ignored.
SUPPORTED_KEYWORDS = frozenset(
    [
        "type",
        "required",
        "properties",
        "additionalProperties",
        "enum",
        "pattern",
        "items",
        "const",
        "$ref",
        "uniqueItems",
        "minItems",
        "minLength",
        "minimum",
        "maximum",
    ]
)
TYPE_NAMES = frozenset(["null", "boolean", "integer", "number", "string", "array", "object"])
_MAX_REF_DEPTH = 64


def _is_type(value, name):
    if name == "null":
        return value is None
    if name == "boolean":
        return isinstance(value, bool)
    if name == "integer":
        if isinstance(value, bool):
            return False
        return isinstance(value, int) or (isinstance(value, float) and value.is_integer())
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if name == "string":
        return isinstance(value, str)
    if name == "array":
        return isinstance(value, list)
    if name == "object":
        return isinstance(value, dict)
    raise SchemaError("unknown type name %r" % (name,))


def json_equal(a, b):
    """JSON equality: true and 1 differ, 1 and 1.0 are equal, containers compare deeply."""
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        return len(a) == len(b) and all(json_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(json_equal(a[k], b[k]) for k in a)
    return a == b


def check_schema_subset(schema, path=""):
    """Return error strings for schema constructs outside the supported subset."""
    if isinstance(schema, bool):
        return []
    if not isinstance(schema, dict):
        return ["%s: a schema must be an object or a boolean" % (path or "/")]
    errors = []
    for key in sorted(schema):
        if key not in SUPPORTED_KEYWORDS and key not in ANNOTATION_KEYWORDS:
            errors.append("%s: unsupported keyword %r" % (path or "/", key))
    if "$ref" in schema:
        ref = schema["$ref"]
        if not isinstance(ref, str) or not (ref == "#" or ref.startswith("#/")):
            errors.append("%s: $ref must be a local '#' or '#/...' pointer" % (path or "/"))
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        for t in types:
            if t not in TYPE_NAMES:
                errors.append("%s: unknown type %r" % (path or "/", t))
    if "pattern" in schema:
        try:
            re.compile(schema["pattern"])
        except (re.error, TypeError) as exc:
            errors.append("%s: bad pattern: %s" % (path or "/", exc))
    if "required" in schema and not (
        isinstance(schema["required"], list) and all(isinstance(r, str) for r in schema["required"])
    ):
        errors.append("%s: required must be a list of strings" % (path or "/"))
    if "enum" in schema and not isinstance(schema["enum"], list):
        errors.append("%s: enum must be a list" % (path or "/"))
    for key in ("properties", "$defs"):
        if key in schema:
            if not isinstance(schema[key], dict):
                errors.append("%s/%s: must be an object" % (path, key))
                continue
            for name in sorted(schema[key]):
                errors.extend(check_schema_subset(schema[key][name], "%s/%s/%s" % (path, key, name)))
    for key in ("items", "additionalProperties"):
        if key in schema:
            errors.extend(check_schema_subset(schema[key], "%s/%s" % (path, key)))
    return errors


def _resolve_ref(root, ref):
    node = root
    if ref == "#":
        return node
    for part in ref[2:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if isinstance(node, dict) and part in node:
            node = node[part]
        elif isinstance(node, list) and part.isdigit() and int(part) < len(node):
            node = node[int(part)]
        else:
            raise SchemaError("unresolvable $ref %r" % ref)
    return node


def _walk(instance, schema, root, path, errors, depth):
    if schema is True:
        return
    if schema is False:
        errors.append((path or "/", "no value is allowed here"))
        return
    if not isinstance(schema, dict):
        raise SchemaError("a schema must be an object or a boolean")
    for key in schema:
        if key not in SUPPORTED_KEYWORDS and key not in ANNOTATION_KEYWORDS:
            raise SchemaError("unsupported keyword %r" % key)
    if "$ref" in schema:
        if depth > _MAX_REF_DEPTH:
            raise SchemaError("$ref nesting is too deep (cycle?)")
        _walk(instance, _resolve_ref(root, schema["$ref"]), root, path, errors, depth + 1)
    here = path or "/"
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(instance, t) for t in types):
            errors.append((here, "expected type %s" % " or ".join(types)))
    if "const" in schema and not json_equal(instance, schema["const"]):
        errors.append((here, "must equal %s" % json.dumps(schema["const"])))
    if "enum" in schema and not any(json_equal(instance, v) for v in schema["enum"]):
        errors.append((here, "must be one of %s" % json.dumps(schema["enum"])))
    if isinstance(instance, str):
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            errors.append((here, "does not match pattern %s" % schema["pattern"]))
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errors.append((here, "shorter than %d characters" % schema["minLength"]))
    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append((here, "below the minimum %s" % schema["minimum"]))
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append((here, "above the maximum %s" % schema["maximum"]))
    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append((here, "fewer than %d items" % schema["minItems"]))
        if schema.get("uniqueItems") is True:
            for j in range(len(instance)):
                for i in range(j):
                    if json_equal(instance[i], instance[j]):
                        errors.append(("%s/%d" % (path, j), "duplicates item %d" % i))
                        break
        if "items" in schema:
            for i, item in enumerate(instance):
                _walk(item, schema["items"], root, "%s/%d" % (path, i), errors, depth)
    if isinstance(instance, dict):
        props = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in instance:
                errors.append((here, "missing required property %r" % name))
        for name in sorted(instance):
            sub = "%s/%s" % (path, name.replace("~", "~0").replace("/", "~1"))
            if name in props:
                _walk(instance[name], props[name], root, sub, errors, depth)
            elif "additionalProperties" in schema:
                if schema["additionalProperties"] is False:
                    errors.append((sub, "unexpected property %r" % name))
                else:
                    _walk(instance[name], schema["additionalProperties"], root, sub, errors, depth)


def validate(instance, schema):
    """Validate instance against schema; return a list of (json_pointer, message) pairs."""
    errors = []
    _walk(instance, schema, schema, "", errors, 0)
    return errors


def is_valid(instance, schema):
    return not validate(instance, schema)


# --- lifecycle FSM: exactly master plan 6.2 ---------------------------------------------------

STATES = (
    "Planned",
    "Specified",
    "InProgress",
    "Verifying",
    "Demonstrated",
    "Accepted",
    "Blocked",
    "Reopened",
)

# Planned > Specified: spec + contract written; Specified > InProgress: dependencies accepted;
# InProgress > Verifying: implementation done; Verifying > InProgress: tests fail;
# Verifying > Demonstrated: tests pass; Demonstrated > Accepted: evidence recorded;
# Accepted > Reopened: regression or contract change; Reopened > InProgress;
# Verifying > Blocked: dependency regressed; Blocked > InProgress.
TRANSITIONS = {
    "Planned": ("Specified",),
    "Specified": ("InProgress",),
    "InProgress": ("Verifying",),
    "Verifying": ("InProgress", "Demonstrated", "Blocked"),
    "Demonstrated": ("Accepted",),
    "Accepted": ("Reopened",),
    "Reopened": ("InProgress",),
    "Blocked": ("InProgress",),
}

# States in which an episode has started work, so its dependencies must be Accepted (6.2).
STARTED = ("InProgress", "Verifying", "Demonstrated", "Accepted")
# Progress order used for waiver ceilings (Blocked and Reopened are not on the main line).
PROGRESS = ("Planned", "Specified", "InProgress", "Verifying", "Demonstrated", "Accepted")
EVIDENCE_STATES = ("Demonstrated", "Accepted")


def fsm_edges():
    """Every legal (from, to) pair, in a stable order."""
    return [(a, b) for a in STATES for b in TRANSITIONS.get(a, ())]


def can_transition(old, new):
    return new in TRANSITIONS.get(old, ())


def next_states(state):
    return list(TRANSITIONS.get(state, ()))


# --- episode helpers --------------------------------------------------------------------------


def meaning_sha256(title, summary):
    """The hash that pins what an episode means: sha256 of 'title\\nsummary' (empty when null)."""
    text = "%s\n%s" % (title, summary if summary is not None else "")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


# --- rule engine ------------------------------------------------------------------------------

RULES = (
    "R-SCHEMA",
    "R-DUP-ID",
    "R-GAP",
    "R-VOLGRAPH",
    "R-DEP-ORDER",
    "R-LIFECYCLE",
    "R-NO-EVIDENCE",
    "R-SILENT-SCOPE",
    "R-DOC-MISSING",
    "R-WO-TRACE",
    "R-VIEW-DRIFT",
    "R-PLAN-SHA",
    "R-DEP-ACCEPTED",
)


def _f(rule, path, message):
    return {"rule": rule, "path": path, "message": message}


def schema_findings(instance, schema_name, base_path="", schema_dir=None):
    schema = load_schema(schema_name, schema_dir)
    errs = ["%s: %s" % (schema_name, e) for e in check_schema_subset(schema)]
    if errs:
        raise SchemaError("; ".join(errs))
    return [
        _f("R-SCHEMA", base_path + p if p != "/" else (base_path or "/"), "%s: %s" % (schema_name, m))
        for p, m in validate(instance, schema)
    ]


def _episode_numbers(ep_id):
    return int(ep_id.rsplit("-", 1)[1])


def _dupes(values):
    seen, dup = set(), []
    for v in values:
        if v in seen and v not in dup:
            dup.append(v)
        seen.add(v)
    return dup


def rule_dup_id(reg, ctx):
    out = []
    ids = [e["id"] for e in reg["episodes"]]
    for d in _dupes(ids):
        idx = [i for i, v in enumerate(ids) if v == d]
        out.append(
            _f("R-DUP-ID", "/episodes/%d" % idx[1], "episode id %s is repeated (also at /episodes/%d)" % (d, idx[0]))
        )
    nums = [e["number"] for e in reg["episodes"]]
    for d in _dupes(nums):
        idx = [i for i, v in enumerate(nums) if v == d]
        out.append(
            _f(
                "R-DUP-ID",
                "/episodes/%d" % idx[1],
                "episode number %d is repeated (also at /episodes/%d)" % (d, idx[0]),
            )
        )
    vids = [v["id"] for v in reg["volumes"]]
    for d in _dupes(vids):
        idx = [i for i, v in enumerate(vids) if v == d]
        out.append(_f("R-DUP-ID", "/volumes/%d" % idx[1], "volume id %s is repeated" % d))
    pnums = [p["number"] for p in reg["parts"]]
    for d in _dupes(pnums):
        idx = [i for i, v in enumerate(pnums) if v == d]
        out.append(_f("R-DUP-ID", "/parts/%d" % idx[1], "part number %d is repeated" % d))
    docs = ctx.get("documents")
    if docs is not None:
        dids = [d["id"] for d in docs["documents"]]
        for d in _dupes(dids):
            idx = [i for i, v in enumerate(dids) if v == d]
            out.append(_f("R-DUP-ID", "/documents/%d" % idx[1], "document id %s is repeated" % d))
    wos = ctx.get("work_orders")
    if wos is not None:
        wids = [w["id"] for w in wos]
        for d in _dupes(wids):
            idx = [i for i, v in enumerate(wids) if v == d]
            out.append(_f("R-DUP-ID", "/work_orders/%d" % idx[1], "work order %s is repeated" % d))
    return out


def _tiles(items, key_first, key_last, base, label, start=1):
    """Consecutive [first, last] ranges must tile start..N with no gap or overlap."""
    out = []
    expect = start
    for i, item in enumerate(items):
        first, last = item[key_first], item[key_last]
        if last < first:
            out.append(_f("R-GAP", "%s/%d" % (base, i), "%s range %d-%d is empty" % (label, first, last)))
        if first != expect:
            out.append(
                _f("R-GAP", "%s/%d" % (base, i), "%s starts at %d but %d is the next number" % (label, first, expect))
            )
        expect = last + 1
    return out, expect - 1


def rule_gap(reg, ctx):
    out = []
    eps = reg["episodes"]
    numbers = [e["number"] for e in eps]
    for i, ep in enumerate(eps):
        if _episode_numbers(ep["id"]) != ep["number"]:
            out.append(_f("R-GAP", "/episodes/%d" % i, "id %s does not match number %d" % (ep["id"], ep["number"])))
    present = set(numbers)
    top = max(numbers) if numbers else 0
    missing = [n for n in range(1, top + 1) if n not in present]
    if missing:
        shown = ", ".join("%03d" % n for n in missing[:10]) + (" ..." if len(missing) > 10 else "")
        out.append(_f("R-GAP", "/episodes", "episode numbers are not contiguous from 001: missing %s" % shown))
    if numbers != sorted(numbers):
        out.append(_f("R-GAP", "/episodes", "episodes are not listed in ascending number order"))
    errs, vol_top = _tiles(reg["volumes"], "first", "last", "/volumes", "volume")
    out.extend(errs)
    if reg["volumes"] and vol_top != top:
        out.append(_f("R-GAP", "/volumes", "volume ranges end at %d but the last episode is %d" % (vol_top, top)))
    errs, part_top = _tiles(reg["parts"], "first", "last", "/parts", "part")
    out.extend(errs)
    if reg["parts"] and part_top != top:
        out.append(_f("R-GAP", "/parts", "part ranges end at %d but the last episode is %d" % (part_top, top)))
    return out


def rule_volgraph(reg, ctx):
    out = []
    vols = {}
    for i, v in enumerate(reg["volumes"]):
        vols.setdefault(v["id"], (i, v))
    parts = {p["number"]: p for p in reg["parts"]}
    for i, v in enumerate(reg["volumes"]):
        part = parts.get(v["part"])
        if part is None:
            out.append(
                _f("R-VOLGRAPH", "/volumes/%d/part" % i, "volume %s names unknown part %d" % (v["id"], v["part"]))
            )
        elif v["first"] < part["first"] or v["last"] > part["last"]:
            out.append(
                _f("R-VOLGRAPH", "/volumes/%d" % i, "volume %s (%d-%d) lies outside part %d (%d-%d)"
                   % (v["id"], v["first"], v["last"], part["number"], part["first"], part["last"]))
            )
        for j, dep in enumerate(v["depends_on_volumes"]):
            if dep == v["id"]:
                out.append(
                    _f("R-VOLGRAPH", "/volumes/%d/depends_on_volumes/%d" % (i, j), "volume %s depends on itself" % dep)
                )
            elif dep not in vols:
                out.append(_f("R-VOLGRAPH", "/volumes/%d/depends_on_volumes/%d" % (i, j), "unknown volume %s" % dep))
    # cycles: iterative depth-first search over the declared edges
    graph = {vid: [d for d in v["depends_on_volumes"] if d in vols and d != vid] for vid, (_, v) in vols.items()}
    state = {}
    for start in sorted(graph):
        if start in state:
            continue
        stack = [(start, iter(sorted(graph[start])))]
        state[start] = 1
        trail = [start]
        while stack:
            node, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                state[node] = 2
                stack.pop()
                trail.pop()
            elif state.get(nxt) == 1:
                cycle = trail[trail.index(nxt):] + [nxt]
                out.append(
                    _f("R-VOLGRAPH", "/volumes/%d" % vols[nxt][0], "volume dependency cycle: %s" % " -> ".join(cycle))
                )
            elif nxt not in state:
                state[nxt] = 1
                trail.append(nxt)
                stack.append((nxt, iter(sorted(graph[nxt]))))
    for i, ep in enumerate(reg["episodes"]):
        entry = vols.get(ep["volume"])
        if entry is None:
            out.append(
                _f(
                    "R-VOLGRAPH",
                    "/episodes/%d/volume" % i,
                    "episode %s names unknown volume %s" % (ep["id"], ep["volume"]),
                )
            )
        elif not entry[1]["first"] <= ep["number"] <= entry[1]["last"]:
            out.append(
                _f("R-VOLGRAPH", "/episodes/%d/volume" % i, "episode %s is outside volume %s (%d-%d)"
                   % (ep["id"], ep["volume"], entry[1]["first"], entry[1]["last"]))
            )
    return out


def _by_id(reg):
    index = {}
    for i, ep in enumerate(reg["episodes"]):
        index.setdefault(ep["id"], (i, ep))
    return index


def rule_dep_order(reg, ctx):
    out = []
    index = _by_id(reg)
    for i, ep in enumerate(reg["episodes"]):
        for j, dep in enumerate(ep["depends_on"]):
            path = "/episodes/%d/depends_on/%d" % (i, j)
            if dep == ep["id"]:
                out.append(_f("R-DEP-ORDER", path, "%s depends on itself" % dep))
            elif dep not in index:
                out.append(_f("R-DEP-ORDER", path, "%s depends on unknown episode %s" % (ep["id"], dep)))
            elif index[dep][1]["number"] >= ep["number"]:
                out.append(
                    _f(
                        "R-DEP-ORDER",
                        path,
                        "%s depends on later episode %s (dependencies must come first)" % (ep["id"], dep),
                    )
                )
    return out


def rule_lifecycle(reg, ctx):
    out = []
    life = reg["lifecycle"]
    if list(life["states"]) != list(STATES):
        out.append(
            _f(
                "R-LIFECYCLE",
                "/lifecycle/states",
                "states differ from master plan 6.2: expected %s" % ", ".join(STATES),
            )
        )
    declared = sorted(tuple(t) for t in life["transitions"])
    if declared != sorted(fsm_edges()):
        out.append(
            _f(
                "R-LIFECYCLE",
                "/lifecycle/transitions",
                "transitions differ from master plan 6.2 (expected %d edges)" % len(fsm_edges()),
            )
        )
    for i, ep in enumerate(reg["episodes"]):
        hist = ep["history"]
        base = "/episodes/%d" % i
        if not hist:
            if ep["status"] != "Planned":
                out.append(
                    _f("R-LIFECYCLE", base + "/status", "%s is %s but has no history" % (ep["id"], ep["status"]))
                )
            continue
        prev_to, prev_date = "Planned", ""
        for j, h in enumerate(hist):
            p = "%s/history/%d" % (base, j)
            if h["from"] != prev_to:
                out.append(
                    _f("R-LIFECYCLE", p, "history does not chain: expected from %s, found %s" % (prev_to, h["from"]))
                )
            if not can_transition(h["from"], h["to"]):
                out.append(
                    _f("R-LIFECYCLE", p, "%s > %s is not a legal transition (master plan 6.2)" % (h["from"], h["to"]))
                )
            if h["date"] < prev_date:
                out.append(_f("R-LIFECYCLE", p, "history date %s is earlier than the previous entry" % h["date"]))
            prev_to, prev_date = h["to"], h["date"]
        if prev_to != ep["status"]:
            out.append(
                _f(
                    "R-LIFECYCLE",
                    base + "/status",
                    "%s status is %s but history ends at %s" % (ep["id"], ep["status"], prev_to),
                )
            )
    return out


def rule_no_evidence(reg, ctx):
    out = []
    for i, ep in enumerate(reg["episodes"]):
        if ep["status"] in EVIDENCE_STATES:
            comp = ep["completion"]
            if comp is None:
                out.append(
                    _f(
                        "R-NO-EVIDENCE",
                        "/episodes/%d/completion" % i,
                        "%s is %s but has no completion record" % (ep["id"], ep["status"]),
                    )
                )
            elif not comp["evidence"]:
                out.append(
                    _f(
                        "R-NO-EVIDENCE",
                        "/episodes/%d/completion/evidence" % i,
                        "%s is %s but lists no evidence" % (ep["id"], ep["status"]),
                    )
                )
    return out


def rule_silent_scope(reg, ctx):
    out = []
    for i, ep in enumerate(reg["episodes"]):
        base = "/episodes/%d" % i
        actual = meaning_sha256(ep["title"], ep["summary"])
        if actual != ep["meaning_sha256"]:
            out.append(
                _f(
                    "R-SILENT-SCOPE",
                    base + "/meaning_sha256",
                    "%s title/summary no longer match meaning_sha256; amend the record instead" % ep["id"],
                )
            )
        prev = None
        for j, am in enumerate(ep["amendments"]):
            p = "%s/amendments/%d" % (base, j)
            if prev is not None and am["from_sha256"] != prev:
                out.append(_f("R-SILENT-SCOPE", p, "amendment does not chain from the previous to_sha256"))
            if am["from_sha256"] == am["to_sha256"]:
                out.append(_f("R-SILENT-SCOPE", p, "amendment changes nothing (from_sha256 equals to_sha256)"))
            if not am["traceability_updated"]:
                out.append(_f("R-SILENT-SCOPE", p, "amendment lacks a traceability update"))
            if ep["decision_status"] == "Locked" and not am["adr"]:
                out.append(_f("R-SILENT-SCOPE", p, "amending a Locked decision requires an ADR"))
            prev = am["to_sha256"]
        if ep["amendments"] and prev != ep["meaning_sha256"]:
            out.append(
                _f("R-SILENT-SCOPE", base + "/amendments", "last amendment to_sha256 does not equal meaning_sha256")
            )
    return out


def rule_doc_missing(reg, ctx):
    docs = ctx.get("documents")
    if docs is None:
        return []
    known = {d["id"] for d in docs["documents"]}
    out = []
    for i, ep in enumerate(reg["episodes"]):
        for j, doc in enumerate(ep["documents"]):
            if doc not in known:
                out.append(
                    _f(
                        "R-DOC-MISSING",
                        "/episodes/%d/documents/%d" % (i, j),
                        "%s names %s which SL-REG-001 does not list" % (ep["id"], doc),
                    )
                )
    return out


def rule_wo_trace(reg, ctx):
    wos = ctx.get("work_orders")
    if wos is None:
        return []
    out = []
    ids = {ep["id"] for ep in reg["episodes"]}
    traced = {}
    for i, wo in enumerate(wos):
        for j, tr in enumerate(wo["traces"]):
            if tr not in ids:
                out.append(
                    _f(
                        "R-WO-TRACE",
                        "/work_orders/%d/traces/%d" % (i, j),
                        "%s traces unknown episode %s" % (wo["id"], tr),
                    )
                )
        traced.setdefault(wo["id"], set()).update(wo["traces"])
    for i, ep in enumerate(reg["episodes"]):
        for j, wid in enumerate(ep["work_orders"]):
            p = "/episodes/%d/work_orders/%d" % (i, j)
            if wid not in traced:
                out.append(_f("R-WO-TRACE", p, "%s lists %s which has no work-order record" % (ep["id"], wid)))
            elif ep["id"] not in traced[wid]:
                out.append(_f("R-WO-TRACE", p, "%s lists %s but %s does not trace it" % (ep["id"], wid, wid)))
    return out


def rule_view_drift(reg, ctx):
    views = ctx.get("views")
    if views is None:
        return []
    out = []
    for name in sorted(views):
        v = views[name]
        if v["expected_sha256"] != v["actual_sha256"]:
            out.append(
                _f(
                    "R-VIEW-DRIFT",
                    "/views/%s" % name,
                    "view differs from the registry-generated content; regenerate it",
                )
            )
    return out


def rule_plan_sha(reg, ctx):
    pinned = ctx.get("plan_sha256")
    if pinned is None:
        return []
    if reg["source"]["sha256"] != pinned:
        return [
            _f(
                "R-PLAN-SHA",
                "/source/sha256",
                "registry source sha256 %s does not equal the pinned plan sha256 %s"
                % (reg["source"]["sha256"][:12], pinned[:12]),
            )
        ]
    return []


def _waiver_for(reg, ep):
    for w in reg["lifecycle"]["waivers"]:
        if w["rule"] == "R-DEP-ACCEPTED" and w["first"] <= ep["number"] <= w["last"]:
            return w
    return None


def rule_dep_accepted(reg, ctx):
    out = []
    index = _by_id(reg)
    for i, ep in enumerate(reg["episodes"]):
        if ep["status"] not in STARTED:
            continue
        waiver = _waiver_for(reg, ep)
        if waiver and PROGRESS.index(ep["status"]) <= PROGRESS.index(waiver["max_status"]):
            continue
        for j, dep in enumerate(ep["depends_on"]):
            target = index.get(dep)
            if target is not None and target[1]["status"] != "Accepted":
                out.append(
                    _f(
                        "R-DEP-ACCEPTED",
                        "/episodes/%d/depends_on/%d" % (i, j),
                        "%s is %s but dependency %s is %s, not Accepted"
                        % (ep["id"], ep["status"], dep, target[1]["status"]),
                    )
                )
    return out


_CHECKS = (
    rule_dup_id,
    rule_gap,
    rule_volgraph,
    rule_dep_order,
    rule_lifecycle,
    rule_no_evidence,
    rule_silent_scope,
    rule_doc_missing,
    rule_wo_trace,
    rule_view_drift,
    rule_plan_sha,
    rule_dep_accepted,
)


def sort_findings(findings):
    order = {r: i for i, r in enumerate(RULES)}
    return sorted(findings, key=lambda f: (order[f["rule"]], f["path"], f["message"]))


def check_registry(registry, documents=None, work_orders=None, plan_sha256=None, views=None, schema_dir=None):
    """Run every rule. Schema findings (R-SCHEMA) are returned alone: the other rules need
    well-formed data. `documents` is an SL-REG-001 object, `work_orders` a list of sl-wo-trace
    records, `plan_sha256` the pinned plan hash, `views` {name: {expected_sha256, actual_sha256}}.
    A context argument left as None skips the rules that need it."""
    findings = schema_findings(registry, "sl-reg-002", schema_dir=schema_dir)
    if documents is not None:
        findings.extend(schema_findings(documents, "sl-reg-001", "/documents_register", schema_dir))
    if work_orders is not None:
        if not isinstance(work_orders, list):
            findings.append(_f("R-SCHEMA", "/work_orders", "work_orders must be a list"))
        else:
            for i, wo in enumerate(work_orders):
                findings.extend(schema_findings(wo, "sl-wo-trace", "/work_orders/%d" % i, schema_dir))
    if views is not None:
        ok = isinstance(views, dict) and all(
            isinstance(v, dict) and set(v) == {"expected_sha256", "actual_sha256"} for v in views.values()
        )
        if not ok:
            findings.append(_f("R-SCHEMA", "/views", "views must map a name to {expected_sha256, actual_sha256}"))
    if findings:
        return sort_findings(findings)
    ctx = {"documents": documents, "work_orders": work_orders, "plan_sha256": plan_sha256, "views": views}
    for check in _CHECKS:
        findings.extend(check(registry, ctx))
    return sort_findings(findings)


BUNDLE_KEYS = ("fixture", "registry", "documents", "work_orders", "plan_sha256", "views")


def check_bundle(bundle, schema_dir=None):
    """Check a fixture bundle: {fixture, registry, documents?, work_orders?, plan_sha256?, views?}."""
    if not isinstance(bundle, dict) or "registry" not in bundle:
        return [_f("R-SCHEMA", "/", "a bundle needs a 'registry' object")]
    extra = sorted(set(bundle) - set(BUNDLE_KEYS))
    if extra:
        return [_f("R-SCHEMA", "/", "unknown bundle keys: %s" % ", ".join(extra))]
    return check_registry(
        bundle["registry"],
        documents=bundle.get("documents"),
        work_orders=bundle.get("work_orders"),
        plan_sha256=bundle.get("plan_sha256"),
        views=bundle.get("views"),
        schema_dir=schema_dir,
    )


def format_finding(f):
    return "%s %s: %s" % (f["rule"], f["path"], f["message"])
