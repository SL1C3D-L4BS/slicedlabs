"""fabric-contract-lock: the contract dump equals both documents and the committed lock (WO-062, FAB-INV-CF-03).

Inputs: the contract dump written by `sl_fabric_dump_contract` (format sl-fabric-contract/1: the id classes, mint
schemes, type kinds, SLB/1 tags, limits, epistemic classes and transitions, error codes, corruption policies,
failpoints and transaction states as the engine compiles them), the `sl-data` tables of SL-ARCH-001 and SL-ARCH-002,
and schemas/fabric/contract.lock.json (format sl-fabric-contract-lock/1: the contract and its SHA-256). Each error
line names its rule and the section and item that differ (an error code by its errc name):

  R-LOCK-DUMP   the dump is unreadable, not sl-fabric-contract/1, or a section is missing or malformed
  R-LOCK-ARCH   a document cannot be read, or no sl-data block holds a table the dump is compared with
  R-LOCK-DOC    a dump section differs from the document table (missing, extra or changed item)
  R-LOCK-LOCK   the lock is unreadable, not sl-fabric-contract-lock/1, or its contract differs from the dump
  R-LOCK-HASH   the lock's sha256 is not the SHA-256 of its contract

Normalisation: the document's type kind groups ("i8|i16|i32|i64") are split, SLB tags compare as numbers, mint schemes
compare scheme and name, transaction states are `tx_states.states`. The lock hash is SHA-256 over the contract as
JSON with sorted keys and no spaces.

  python3 -I tools/fabric/contract_lock.py check <contract.json> [--root DIR] [--lock PATH]
  python3 -I tools/fabric/contract_lock.py write <contract.json> [--root DIR] [--lock PATH]

`check` ends with `fabric-contract-lock: ok` (exit 0) or `failed` (exit 1); `write` checks the documents, then writes
the lock (exit 0) or fails as `check` does. Exit 2 on bad usage. Stdlib only, no subprocess (ADR-0004).
"""

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "governance"))
import cmd_constitution_check  # noqa: E402

DUMP_FORMAT = "sl-fabric-contract/1"
LOCK_FORMAT = "sl-fabric-contract-lock/1"
LOCK_PATH = "schemas/fabric/contract.lock.json"
SECTIONS = ("id_classes", "mint_schemes", "type_kinds", "slb_tags", "limits", "epistemic", "error_codes",
            "corruption", "failpoints", "tx_states")
DOCS = ("SL-ARCH-001-", "SL-ARCH-002-")
USAGE = "usage: contract_lock.py check|write <contract.json> [--root DIR] [--lock PATH]"


def load_docs(root):
    """Return (tables, errors): every sl-data key of both documents, merged."""
    arch_dir = os.path.join(root, "docs", "arch")
    names = sorted(os.listdir(arch_dir)) if os.path.isdir(arch_dir) else []
    tables, errors = {}, []
    for stem in DOCS:
        found = [n for n in names if n.startswith(stem) and n.endswith(".md")]
        if not found:
            errors.append("docs/arch/%s*.md: R-LOCK-ARCH: the document is missing" % stem)
            continue
        rel = "docs/arch/" + found[0]
        try:
            with open(os.path.join(arch_dir, found[0]), encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError) as exc:
            errors.append("%s: R-LOCK-ARCH: unreadable: %s" % (rel, exc))
            continue
        blocks, bad = cmd_constitution_check.sl_data_blocks(text)
        errors.extend("%s: R-LOCK-ARCH: %s" % (rel, b) for b in bad)
        for block in blocks:
            if isinstance(block, dict):
                tables.update(block)
    return tables, errors


def doc_contract(tables):
    """Return (contract, errors): the document tables in dump form."""
    need = ("id_classes", "mint_schemes", "type_kinds", "slb_tags", "limits", "epistemic", "error_codes",
            "corruption", "failpoints", "tx_states")
    errors = ["docs/arch: R-LOCK-ARCH: no sl-data block holds `%s`" % k for k in need if k not in tables]
    if errors:
        return None, errors
    try:
        out = {
            "id_classes": [{"class": r["class"], "name": r["name"], "prefix": r["prefix"]}
                           for r in tables["id_classes"]],
            "mint_schemes": [{"scheme": r["scheme"], "name": r["name"]} for r in tables["mint_schemes"]],
            "type_kinds": [k for r in tables["type_kinds"] for k in r["kind"].split("|")],
            "slb_tags": [{"tag": int(r["tag"], 16), "name": r["name"]} for r in tables["slb_tags"]],
            "limits": dict(tables["limits"]),
            "epistemic": {"classes": list(tables["epistemic"]["classes"]),
                          "transitions": [{"from": t["from"], "to": t["to"]}
                                          for t in tables["epistemic"]["transitions"]]},
            "error_codes": [[name, number] for name, number in tables["error_codes"]],
            "corruption": [{k: r[k] for k in ("class", "name", "detect", "policy", "evidence")}
                           for r in tables["corruption"]],
            "failpoints": [{k: r[k] for k in ("fp", "at", "outcome")} for r in tables["failpoints"]],
            "tx_states": list(tables["tx_states"]["states"]),
        }
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        return None, ["docs/arch: R-LOCK-ARCH: a contract table is malformed: %r" % (exc,)]
    return out, []


def normalise_dump(dump):
    """Return (contract, errors): the dump sections, tags as numbers."""
    if not isinstance(dump, dict) or dump.get("format") != DUMP_FORMAT:
        return None, ["R-LOCK-DUMP: the dump is not %s" % DUMP_FORMAT]
    missing = [s for s in SECTIONS if s not in dump]
    if missing:
        return None, ["R-LOCK-DUMP: the dump lacks %s" % ", ".join(missing)]
    out = {s: dump[s] for s in SECTIONS}
    try:
        out["slb_tags"] = [{"tag": int(r["tag"], 16), "name": r["name"]} for r in dump["slb_tags"]]
        out["error_codes"] = [[n, v] for n, v in dump["error_codes"]]
    except (KeyError, TypeError, ValueError) as exc:
        return None, ["R-LOCK-DUMP: a dump section is malformed: %r" % (exc,)]
    return out, []


def item_key(section, item):
    """A readable name of one item for an error line."""
    if section == "error_codes":
        return "errc %s" % item[0]
    if isinstance(item, dict):
        for k in ("class", "scheme", "fp", "tag", "from"):
            if k in item:
                return "%s %s" % (k, item[k])
    return json.dumps(item, sort_keys=True)


def compare(rule, section, code, other, what):
    """Error lines for section `section` where `code` (the dump) differs from `other` (the docs or the lock)."""
    if code == other:
        return []
    if section == "error_codes":
        mine, theirs = dict(code), dict(other)
        lines = []
        for name in sorted(set(mine) | set(theirs)):
            if name not in theirs:
                lines.append("%s: error_codes: errc %s = %d is in the code, not in %s" % (rule, name, mine[name], what))
            elif name not in mine:
                lines.append("%s: error_codes: errc %s = %d is in %s, not in the code"
                             % (rule, name, theirs[name], what))
            elif mine[name] != theirs[name]:
                lines.append("%s: error_codes: errc %s is %d in the code, %d in %s" % (rule, name, mine[name],
                                                                                     theirs[name], what))
        return lines or ["%s: error_codes: the order differs from %s" % (rule, what)]
    if isinstance(code, list) and isinstance(other, list):
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


def contract_sha(contract):
    text = json.dumps(contract, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def check(dump_path, root, lock_path, with_lock=True):
    """Return (lines, contract): every error line, and the normalised dump (None when unreadable)."""
    try:
        dump = read_json(dump_path)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        return ["R-LOCK-DUMP: %s: unreadable: %s" % (dump_path, exc)], None
    contract, lines = normalise_dump(dump)
    if contract is None:
        return lines, None
    tables, errors = load_docs(root)
    lines.extend(errors)
    docs, errors = doc_contract(tables)
    lines.extend(errors)
    if docs is not None:
        for s in SECTIONS:
            lines.extend(compare("R-LOCK-DOC", s, contract[s], docs[s], "the documents"))
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
    """The lock as JSON with one table item per line (sorted keys), so a review diff names the item."""
    lines = ["{", '  "format": %s,' % json.dumps(LOCK_FORMAT), '  "sha256": %s,' % json.dumps(contract_sha(contract)),
             '  "contract": {']
    for n, section in enumerate(sorted(contract)):
        value = contract[section]
        tail = "," if n + 1 < len(contract) else ""
        if isinstance(value, list):
            items = [json.dumps(v, sort_keys=True) for v in value]
            lines.append('    %s: [' % json.dumps(section))
            lines.extend("      %s%s" % (item, "," if i + 1 < len(items) else "") for i, item in enumerate(items))
            lines.append("    ]%s" % tail)
        else:
            lines.append("    %s: %s%s" % (json.dumps(section), json.dumps(value, sort_keys=True), tail))
    lines += ["  }", "}", ""]
    return "\n".join(lines)


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
    if len(args) != 2 or args[0] not in ("check", "write"):
        print(USAGE, file=sys.stderr)
        return 2
    lock = lock or os.path.join(root, *LOCK_PATH.split("/"))
    lines, contract = check(args[1], root, lock, with_lock=args[0] == "check")
    for line in lines:
        print(line)
    if args[0] == "write" and not lines:
        write_lock(contract, lock)
        print("fabric-contract-lock: wrote %s" % os.path.relpath(lock, root))
        return 0
    print("fabric-contract-lock: %s" % ("failed" if lines else "ok"))
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
