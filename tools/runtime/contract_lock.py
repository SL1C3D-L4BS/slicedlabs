"""runtime-contract-lock: the runtime contract equals SL-ARCH-003 and the committed lock (WO-100, RT-INV-GATE-06).

Inputs: the dump written by `sl_runtime_dump_contract` (format sl-runtime-contract/1: rt_states, failpoints, phases,
boundary_slots, limits, error_codes, gate evidence tags and table versions as sl_runtime compiles them), the sections
the Python tools own (stress_matrix from tools/runtime/stress_report.py CELLS, the evidence formats of
determinism_check.py, stress_report.py and scorecard.py, and layers from the header files under
engine/runtime/include/sl/runtime), the `sl-data` blocks of SL-ARCH-003 and schemas/runtime/contract.lock.json (format
sl-runtime-contract-lock/1: the contract and its SHA-256). Each error line names its rule, the section and the item
that differs (an error code by its errc name, a limit by its name):

  R-LOCK-DUMP   the dump is unreadable, not sl-runtime-contract/1, or a section is missing or malformed
  R-LOCK-ARCH   SL-ARCH-003 cannot be read, or no sl-data block holds a table the contract is compared with
  R-LOCK-DOC    a contract section differs from the document table (missing, extra or changed item)
  R-LOCK-LOCK   the lock is unreadable, not sl-runtime-contract-lock/1, or its contract differs from this one
  R-LOCK-HASH   the lock's sha256 is not the SHA-256 of its contract

Normalisation: rt_states transitions compare as sorted (from, event, to) triples (the document's fourth column is
prose); failpoints compare id, name and site (the outcome is prose the tests check); a `limits` row with
"bound": false is a process or test-tier number the code cannot expose and is skipped, and the code must not define
it; gate_evidence compares the rows whose format is not "text"; stress_matrix compares cell and test file; layers
compare the header list (the parenthetical of a row is prose). The lock hash is SHA-256 over the contract as JSON
with sorted keys and no spaces.

  python3 -I tools/runtime/contract_lock.py check [contract-dump.json] [--root DIR] [--lock PATH]
  python3 -I tools/runtime/contract_lock.py write [contract-dump.json] [--root DIR] [--lock PATH]

The dump defaults to build/runtime/contract-dump.json under --root. `check` ends with `runtime-contract-lock: ok`
(exit 0) or `failed` (exit 1); `write` checks the document, then writes the lock (exit 0) or fails as `check` does;
no acceptance command runs `write` (ARCH3-R015). Exit 2 on bad usage. Stdlib only, no subprocess (ADR-0004).
"""

import hashlib
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "governance"))
import cmd_constitution_check  # noqa: E402


def _sibling(name):
    """tools/runtime/<name>.py under a unique module name: tools/fabric has modules of the same names (tools-test)."""
    key = "runtime_" + name
    if key not in sys.modules:
        spec = importlib.util.spec_from_file_location(key, os.path.join(HERE, name + ".py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[key] = module
        spec.loader.exec_module(module)
    return sys.modules[key]


determinism_check = _sibling("determinism_check")
scorecard = _sibling("scorecard")
stress_report = _sibling("stress_report")

DUMP_FORMAT = "sl-runtime-contract/1"
LOCK_FORMAT = "sl-runtime-contract-lock/1"
LOCK_PATH = "schemas/runtime/contract.lock.json"
DUMP_PATH = "build/runtime/contract-dump.json"
DOC_STEM = "SL-ARCH-003-"
HEADER_DIR = "engine/runtime/include/sl/runtime"
DUMP_SECTIONS = ("boundary_slots", "error_codes", "failpoints", "gate_evidence", "limits", "phases", "rt_states",
                 "table_versions")
SECTIONS = DUMP_SECTIONS + ("layers", "stress_matrix")
DOC_KEYS = ("rt_states", "failpoints", "phases", "limits", "error_codes", "gate_evidence", "stress_matrix", "layers")
USAGE = "usage: contract_lock.py check|write [contract-dump.json] [--root DIR] [--lock PATH]"


def load_blocks(root):
    """Return (rel, blocks, errors): every sl-data block (a dict) of SL-ARCH-003, in document order."""
    arch_dir = os.path.join(root, "docs", "arch")
    names = sorted(os.listdir(arch_dir)) if os.path.isdir(arch_dir) else []
    found = [n for n in names if n.startswith(DOC_STEM) and n.endswith(".md")]
    if not found:
        return "docs/arch/%s*.md" % DOC_STEM, [], ["docs/arch/%s*.md: R-LOCK-ARCH: the document is missing" % DOC_STEM]
    rel = "docs/arch/" + found[0]
    try:
        with open(os.path.join(arch_dir, found[0]), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return rel, [], ["%s: R-LOCK-ARCH: unreadable: %s" % (rel, exc)]
    blocks, bad = cmd_constitution_check.sl_data_blocks(text)
    return rel, [b for b in blocks if isinstance(b, dict)], ["%s: R-LOCK-ARCH: %s" % (rel, b) for b in bad]


def block_with(blocks, key):
    """The first block holding `key`, or None."""
    for block in blocks:
        if key in block:
            return block
    return None


def _layer_headers(text):
    """The header names of one `layers` row: comma separated, a parenthetical dropped."""
    return [h.strip() for h in re.sub(r"\([^)]*\)", "", text).split(",") if h.strip()]


def doc_contract(rel, blocks):
    """Return (contract, bound_false, errors): the document tables in contract form."""
    errors = ["%s: R-LOCK-ARCH: no sl-data block holds `%s`" % (rel, k) for k in DOC_KEYS
              if block_with(blocks, k) is None]
    if errors:
        return None, set(), errors
    try:
        rt = block_with(blocks, "rt_states")["rt_states"]
        limits, unbound = {}, set()
        for row in block_with(blocks, "limits")["limits"]:
            if row.get("bound", True) is False:
                unbound.add(row["name"])
            else:
                limits[row["name"]] = row["value"]
        phases_block = block_with(blocks, "phases")
        out = {
            "boundary_slots": list(phases_block["boundary_slots"]),
            "error_codes": [{"area": r["area"], "name": r["name"], "number": r["number"]}
                            for r in block_with(blocks, "error_codes")["error_codes"]],
            "failpoints": [{"id": r["id"], "name": r["name"], "site": r["site"]}
                           for r in block_with(blocks, "failpoints")["failpoints"]],
            "gate_evidence": sorted(({"file": r["file"], "format": r["format"]}
                                     for r in block_with(blocks, "gate_evidence")["gate_evidence"]
                                     if r["format"] != "text"), key=lambda r: r["file"]),
            "layers": [{"layer": r["layer"], "headers": _layer_headers(r["headers"])}
                       for r in block_with(blocks, "layers")["layers"]],
            "limits": limits,
            "phases": [{"index": r["index"], "phase": r["phase"]} for r in phases_block["phases"]],
            "rt_states": {"events": list(rt["events"]), "states": list(rt["states"]),
                          "transitions": sorted([t[0], t[1], t[2]] for t in rt["transitions"])},
            "stress_matrix": [{"cell": r["cell"], "test": r["test"]}
                              for r in block_with(blocks, "stress_matrix")["stress_matrix"]],
            "table_versions": {"error_codes": block_with(blocks, "error_codes")["table_version"],
                               "failpoints": block_with(blocks, "failpoints")["table_version"]},
        }
    except (KeyError, TypeError, ValueError, AttributeError, IndexError) as exc:
        return None, set(), ["%s: R-LOCK-ARCH: a contract table is malformed: %r" % (rel, exc)]
    return out, unbound, []


def python_sections(root):
    """The sections the Python tools and the file tree own."""
    headers = []
    base = os.path.join(root, *HEADER_DIR.split("/"))
    for dirpath, _dirs, files in os.walk(base):
        for name in files:
            if name.endswith(".hpp"):
                headers.append(os.path.relpath(os.path.join(dirpath, name), base).replace(os.sep, "/"))
    return {
        "headers": sorted(headers),
        "stress_matrix": [{"cell": cell, "test": stress_report.test_path(stem)}
                          for cell, (stem, _seed) in stress_report.CELLS.items()],
        "gate_evidence": [{"file": "../scorecard.json", "format": scorecard.OUTPUT_FORMAT},
                          {"file": "hash-matrix.json", "format": determinism_check.FORMAT},
                          {"file": "stress-matrix.json", "format": stress_report.FORMAT}],
    }


def build_contract(dump, root, doc_layers):
    """Return (contract, errors): the dump sections plus the Python-owned ones, in contract form."""
    if not isinstance(dump, dict) or dump.get("format") != DUMP_FORMAT:
        return None, ["R-LOCK-DUMP: the dump is not %s" % DUMP_FORMAT]
    missing = [s for s in DUMP_SECTIONS if s not in dump]
    if missing:
        return None, ["R-LOCK-DUMP: the dump lacks %s" % ", ".join(missing)]
    py = python_sections(root)
    try:
        out = {s: dump[s] for s in DUMP_SECTIONS}
        out["rt_states"] = {"events": list(dump["rt_states"]["events"]), "states": list(dump["rt_states"]["states"]),
                            "transitions": sorted(list(t) for t in dump["rt_states"]["transitions"])}
        out["gate_evidence"] = sorted(list(dump["gate_evidence"]) + py["gate_evidence"], key=lambda r: r["file"])
    except (KeyError, TypeError) as exc:
        return None, ["R-LOCK-DUMP: a dump section is malformed: %r" % (exc,)]
    out["stress_matrix"] = py["stress_matrix"]
    # layers: the document's layer assignment, which must cover exactly the header files (checked in compare)
    out["layers"] = doc_layers if doc_layers is not None else []
    out["headers"] = py["headers"]
    return out, []


def item_key(section, item):
    """A readable name of one item for an error line."""
    if isinstance(item, dict):
        for k in KEYS:
            if k in item:
                prefix = "errc" if section == "error_codes" and k == "name" else k
                return "%s %s" % (prefix, item[k])
    return json.dumps(item, sort_keys=True)


KEYS = ("id", "name", "cell", "file", "phase", "layer")


def _key_of(items):
    """The first field of KEYS that every item (a dict) has and that is unique, or None."""
    if not items or not all(isinstance(i, dict) for i in items):
        return None
    for k in KEYS:
        if all(k in i for i in items) and len({json.dumps(i[k]) for i in items}) == len(items):
            return k
    return None


def keyed_compare(rule, section, code, other, what):
    """Lines for two lists of records matched by key (missing, extra, changed field, order), or None."""
    k = _key_of(code)
    if k is None or _key_of(other) != k:
        return None
    mine = {i[k]: i for i in code}
    theirs = {i[k]: i for i in other}
    lines = []
    for name in [i[k] for i in code] + [i[k] for i in other if i[k] not in mine]:
        label = item_key(section, mine.get(name) or theirs[name])
        if name not in theirs:
            lines.append("%s: %s: %s is in the code, not in %s: %s" % (rule, section, label, what,
                                                                     json.dumps(mine[name], sort_keys=True)))
        elif name not in mine:
            lines.append("%s: %s: %s is in %s, not in the code: %s" % (rule, section, label, what,
                                                                     json.dumps(theirs[name], sort_keys=True)))
        else:
            for f in sorted(set(mine[name]) | set(theirs[name])):
                if mine[name].get(f) != theirs[name].get(f):
                    lines.append("%s: %s: %s: %s is %s in the code, %s in %s"
                                 % (rule, section, label, f, json.dumps(mine[name].get(f)),
                                    json.dumps(theirs[name].get(f)), what))
    if not lines and [i[k] for i in code] != [i[k] for i in other]:
        lines.append("%s: %s: the order differs from %s" % (rule, section, what))
    return lines


def compare(rule, section, code, other, what):
    """Error lines for `section` where `code` (the contract) differs from `other` (the document or the lock)."""
    if code == other:
        return []
    if section == "error_codes":
        mine = {r["name"]: r for r in code}
        theirs = {r["name"]: r for r in other}
        lines = []
        for name in sorted(set(mine) | set(theirs)):
            if name not in theirs:
                lines.append("%s: error_codes: errc %s = %d is in the code, not in %s" % (rule, name,
                                                                                      mine[name]["number"], what))
            elif name not in mine:
                lines.append("%s: error_codes: errc %s = %d is in %s, not in the code"
                             % (rule, name, theirs[name]["number"], what))
            elif mine[name] != theirs[name]:
                for key in ("number", "area"):
                    if mine[name][key] != theirs[name][key]:
                        lines.append("%s: error_codes: errc %s %s is %s in the code, %s in %s"
                                     % (rule, name, key, mine[name][key], theirs[name][key], what))
        return lines or ["%s: error_codes: the order differs from %s" % (rule, what)]
    if isinstance(code, dict) and isinstance(other, dict):
        lines = []
        if section == "rt_states":
            a = {tuple(t) for t in code.get("transitions", [])}
            b = {tuple(t) for t in other.get("transitions", [])}
            lines += ["%s: rt_states: transition %s is in the code, not in %s" % (rule, json.dumps(list(t)), what)
                      for t in sorted(a - b)]
            lines += ["%s: rt_states: transition %s is in %s, not in the code" % (rule, json.dumps(list(t)), what)
                      for t in sorted(b - a)]
            code = {k: v for k, v in code.items() if k != "transitions"}
            other = {k: v for k, v in other.items() if k != "transitions"}
        for key in sorted(set(code) | set(other)):
            label = "limit" if section == "limits" else section
            if key not in other:
                lines.append("%s: %s: %s %s = %s is in the code, not in %s"
                             % (rule, section, label, key, json.dumps(code[key]), what))
            elif key not in code:
                lines.append("%s: %s: %s %s = %s is in %s, not in the code"
                             % (rule, section, label, key, json.dumps(other[key]), what))
            elif code[key] != other[key]:
                lines.append("%s: %s: %s %s is %s in the code, %s in %s"
                             % (rule, section, label, key, json.dumps(code[key], sort_keys=True), json.dumps(
                                 other[key], sort_keys=True), what))
        return lines
    if isinstance(code, list) and isinstance(other, list):
        keyed = keyed_compare(rule, section, code, other, what)
        if keyed is not None:
            return keyed
        lines = []
        for i in range(max(len(code), len(other))):
            a = code[i] if i < len(code) else None
            b = other[i] if i < len(other) else None
            if a != b:
                if a is None:
                    lines.append("%s: %s: %s is in %s, not in the code" % (rule, section, item_key(section, b), what))
                elif b is None:
                    lines.append("%s: %s: %s is in the code, not in %s" % (rule, section, item_key(section, a), what))
                else:
                    lines.append("%s: %s: item %d: code %s, %s %s" % (rule, section, i, json.dumps(a, sort_keys=True),
                                                                    what, json.dumps(b, sort_keys=True)))
        return lines
    return ["%s: %s: code %s, %s %s" % (rule, section, json.dumps(code, sort_keys=True), what,
                                         json.dumps(other, sort_keys=True))]


def layer_errors(rel, layers, headers):
    """Every header file in exactly one layer row, and every layer header a file."""
    lines, seen = [], {}
    for row in layers:
        for h in row["headers"]:
            seen.setdefault(h, []).append(row["layer"])
    for h in headers:
        if h not in seen:
            lines.append("R-LOCK-DOC: layers: header %s is in the code, not in the `layers` table of %s" % (h, rel))
        elif len(seen[h]) > 1:
            lines.append("R-LOCK-DOC: layers: header %s is in more than one layer (%s)" % (h, ", ".join(seen[h])))
    for h in sorted(set(seen) - set(headers)):
        lines.append("R-LOCK-DOC: layers: header %s is in the `layers` table, not in the code" % h)
    return lines


def contract_sha(contract):
    text = json.dumps(contract, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def check(dump_path, root, lock_path, with_lock=True):
    """Return (lines, contract): every error line, and the contract (None when the dump is unreadable)."""
    try:
        dump = read_json(dump_path)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        return ["R-LOCK-DUMP: %s: unreadable: %s" % (dump_path, exc)], None
    rel, blocks, lines = load_blocks(root)
    docs, unbound, errors = doc_contract(rel, blocks) if blocks else (None, set(), [])
    lines.extend(errors)
    if not blocks and not lines:
        lines.append("%s: R-LOCK-ARCH: no sl-data block" % rel)
    contract, errors = build_contract(dump, root, docs["layers"] if docs else None)
    lines.extend(errors)
    if contract is None:
        return lines, None
    headers = contract.pop("headers")
    if docs is not None:
        for name in sorted(unbound & set(contract["limits"])):
            lines.append("R-LOCK-DOC: limits: limit %s is \"bound\": false in the document but the code defines it"
                         % name)
        code_limits = {k: v for k, v in contract["limits"].items() if k not in unbound}
        for s in SECTIONS:
            if s == "layers":
                lines.extend(layer_errors(rel, docs["layers"], headers))
            elif s == "limits":
                lines.extend(compare("R-LOCK-DOC", s, code_limits, docs[s], "the document"))
            else:
                lines.extend(compare("R-LOCK-DOC", s, contract[s], docs[s], "the document"))
    if not with_lock:
        return lines, contract
    try:
        lock = read_json(lock_path)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        lines.append("R-LOCK-LOCK: %s: unreadable: %s" % (lock_path, exc))
        return lines, contract
    if not isinstance(lock, dict) or lock.get("format") != LOCK_FORMAT or not isinstance(lock.get("contract"), dict):
        lines.append("R-LOCK-LOCK: %s: not %s" % (lock_path, LOCK_FORMAT))
        return lines, contract
    locked = lock["contract"]
    if lock.get("sha256") != contract_sha(locked):
        lines.append("R-LOCK-HASH: %s: sha256 is not the SHA-256 of its contract" % lock_path)
    for s in SECTIONS:
        if s not in locked:
            lines.append("R-LOCK-LOCK: %s: the lock lacks section %s" % (lock_path, s))
        else:
            lines.extend(compare("R-LOCK-LOCK", s, contract[s], locked[s], "the lock"))
    return lines, contract


def lock_text(contract):
    """The lock as canonical JSON (indent 2, as `just fmt-check-all` wants), contract sections and keys sorted."""
    ordered = json.loads(json.dumps(contract, sort_keys=True))
    doc = {"format": LOCK_FORMAT, "sha256": contract_sha(contract), "contract": ordered}
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def write_lock(contract, lock_path):
    os.makedirs(os.path.dirname(os.path.abspath(lock_path)), exist_ok=True)
    with open(lock_path, "w", encoding="utf-8") as fh:
        fh.write(lock_text(contract))


def main(argv):
    args = list(argv)
    root, lock = ROOT, None
    for flag in ("--root", "--lock"):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                print(USAGE, file=sys.stderr)
                return 2
            if flag == "--root":
                root = os.path.abspath(args[i + 1])
            else:
                lock = os.path.abspath(args[i + 1])
            del args[i:i + 2]
    if len(args) not in (1, 2) or args[0] not in ("check", "write"):
        print(USAGE, file=sys.stderr)
        return 2
    dump = args[1] if len(args) == 2 else os.path.join(root, *DUMP_PATH.split("/"))
    lock = lock or os.path.join(root, *LOCK_PATH.split("/"))
    lines, contract = check(dump, root, lock, with_lock=args[0] == "check")
    for line in lines:
        print(line)
    if args[0] == "write" and not lines:
        write_lock(contract, lock)
        print("runtime-contract-lock: wrote %s" % os.path.relpath(lock, root))
        return 0
    print("runtime-contract-lock: %s" % ("failed" if lines else "ok"))
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
