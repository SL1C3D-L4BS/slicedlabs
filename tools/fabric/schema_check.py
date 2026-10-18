"""fabric-schema-check (WO-040, ADR-0013, ADR-0017, SL-ARCH-001 Schemas and versioning and Ownership).

Checks every `schemas/fabric/*.sltype.json` module against `schemas/fabric/sl-type-def.schema.json`, the
released-version lock `schemas/fabric/schema.lock.json` and the domain registry of SL-ARCH-001. Each error line is
`<file>: <RULE>: <type> <message>`. Rules:

  R-SCH-FORMAT      a module is not valid JSON or not valid sl-type-def/1 (JSON Schema findings with pointer)
  R-SCH-NAME        file name is not <module>.sltype.json, a type name lacks the `<module>.` prefix, or a name repeats
  R-SCH-WELLFORMED  duplicate numbers or names, unresolved type, kind/shape mismatch, bad default or ref, unbounded
                    recursion (a struct reaches itself only through required fields, variant cases or tagged bases)
  R-SCH-LOCK        schema.lock.json is malformed or an entry's hash does not match its definition
  R-SCH-RELEASED    a released (type, version) whose hash changed, whose version fell, or that left the sources
  R-SCH-BUMP        the change class needs a bigger version bump than the one given (identical needs a minor too:
                    any edit changes the object id)
  R-SCH-REUSE       a reserved field number is a field again (or left the reserved list)
  R-SCH-RESERVE     a field was removed without reserving its number
  R-SCH-FORBIDDEN   domain, state class or record class of a released type changed
  R-SCH-TRANSIENT   state class outside document, derived, observation
  R-OWN-DUP         a domain has two owners: two modules, or a module against the domain_registry
  R-OWN-ORPHAN      a module or type domain is not under a reserved prefix (or fabric domain), or outside its module
  R-OWN-STATE       a state class the domain_registry does not admit for the prefix (e.g. a document under runtime)
  R-SCH-ARCH        SL-ARCH-001 cannot be read

Zero modules is a pass. `lock` pins every (type, version) not yet released (never edits an existing entry) after a clean
check. Run as `python3 -I tools/fabric/schema_check.py [check|lock] [--root DIR]`; the output ends with
`modules M types T released R`.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema_compat  # noqa: E402
import schemalib  # noqa: E402
from schemalib import SCALARS, INT_RANGES  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "governance"))
import slib  # noqa: E402

DIR_REL = "schemas/fabric"
LOCK_FORMAT = "sl-schema-lock/1"
RULE_OF_CODE = {
    "reuse_reserved": "R-SCH-REUSE", "remove_unreserved": "R-SCH-RESERVE",
    "change_domain": "R-SCH-FORBIDDEN", "change_state_class": "R-SCH-FORBIDDEN",
    "change_record_class": "R-SCH-FORBIDDEN",
}


def _err(rel, rule, subject, message):
    return "%s: %s: %s%s" % (rel, rule, subject + " " if subject else "", message)


# ---- loading ---------------------------------------------------------------------------------------------------

def load_modules(root):
    """Return (modules, errors): modules is a list of (relative path, module dict) that passed the JSON Schema."""
    directory = os.path.join(root, *DIR_REL.split("/"))
    errors, modules = [], []
    try:
        schema = schemalib.load_type_def_schema(root)
    except slib.LoadError as exc:
        return [], ["%s: R-SCH-FORMAT: %s" % (schemalib.SCHEMA_REL, exc)]
    for problem in slib.check_schema_subset(schema):
        errors.append("%s: R-SCH-FORMAT: schema outside the validator subset: %s" % (schemalib.SCHEMA_REL, problem))
    for name in schemalib.module_files(directory):
        rel = "%s/%s" % (DIR_REL, name)
        try:
            module = slib.load_json(os.path.join(directory, name))
        except slib.LoadError as exc:
            errors.append("%s: R-SCH-FORMAT: %s" % (rel, exc))
            continue
        found = schemalib.validate_module(module, schema)
        errors.extend("%s: R-SCH-FORMAT: %s: %s" % (rel, pointer, message) for pointer, message in found)
        if found:
            continue
        if name != module["module"] + schemalib.MODULE_SUFFIX:
            errors.append(_err(rel, "R-SCH-NAME", "", "file name must be %s%s" % (module["module"],
                                                                                    schemalib.MODULE_SUFFIX)))
        modules.append((rel, module))
    return modules, errors


def load_lock(root):
    """Return (lock, errors); the lock is {"format", "released": {type: {version: {"hash", "definition"}}}}."""
    rel = "%s/schema.lock.json" % DIR_REL
    try:
        lock = slib.load_json(os.path.join(root, *rel.split("/")))
    except slib.LoadError as exc:
        return None, ["%s: R-SCH-LOCK: %s" % (rel, exc)]
    errors = []
    if not isinstance(lock, dict) or set(lock) != {"format", "released"} or lock.get("format") != LOCK_FORMAT \
            or not isinstance(lock.get("released"), dict):
        return None, ['%s: R-SCH-LOCK: expected {"format": "%s", "released": {...}}' % (rel, LOCK_FORMAT)]
    for name in sorted(lock["released"]):
        versions = lock["released"][name]
        if not isinstance(versions, dict) or not versions:
            errors.append(_err(rel, "R-SCH-LOCK", name, "must map versions to entries"))
            continue
        for version in sorted(versions):
            entry = versions[version]
            subject = "%s %s" % (name, version)
            if not isinstance(entry, dict) or set(entry) != {"hash", "definition"} \
                    or not isinstance(entry["definition"], dict):
                errors.append(_err(rel, "R-SCH-LOCK", subject, 'entry must be {"hash", "definition"}'))
                continue
            d = entry["definition"]
            try:
                same = d.get("name") == name and schemalib.version_text(d) == version
                actual = schemalib.lock_hash(d) if same else None
            except (KeyError, TypeError, schemalib.SchemaLibError) as exc:
                errors.append(_err(rel, "R-SCH-LOCK", subject, "definition cannot be hashed: %s" % exc))
                continue
            if not same:
                errors.append(_err(rel, "R-SCH-LOCK", subject, "definition names another type or version"))
            elif actual != entry["hash"]:
                errors.append(_err(rel, "R-SCH-LOCK", subject, "hash %s does not match its definition (%s)"
                                   % (entry["hash"], actual)))
    return lock, errors


# ---- module rules ----------------------------------------------------------------------------------------------

def _unique(items, key, rel, subject, what, errors):
    seen = {}
    for item in items:
        seen.setdefault(item[key], 0)
        seen[item[key]] += 1
    for value in sorted(seen, key=str):
        if seen[value] > 1:
            errors.append(_err(rel, "R-SCH-WELLFORMED", subject, "%s %s is used %d times" % (what, value, seen[value])))


def _resolve(known, name, allow_ref=False):
    """kind of a type name: scalar | ref | the known type dict | None."""
    if name == schemalib.REF_TYPE:
        return "ref" if allow_ref else None
    if name in SCALARS:
        return "scalar"
    return known.get(name)


def _check_type(rel, module, t, known, arch, errors):
    subject = "%s %s" % (t["name"], t["version"])
    kind = t["kind"]
    add = errors.append

    def bad(message):
        add(_err(rel, "R-SCH-WELLFORMED", subject, message))

    if not t["name"].startswith(module["module"] + "."):
        add(_err(rel, "R-SCH-NAME", subject, "type name must start with the module name %s." % module["module"]))
    if t["state_class"] not in schemalib.STATE_CLASSES:
        add(_err(rel, "R-SCH-TRANSIENT", subject, "state class %r is not document, derived or observation "
                 "(transient state is not representable)" % t["state_class"]))
    record = t["record_class"] != "none"
    if record and t["record_class"] not in arch["id_classes"]:
        bad("record class %r is neither none nor an id class" % t["record_class"])
    if record and kind != "struct":
        bad("a record type must be a struct, not %s" % kind)
    if t.get("document_root") and not record:
        bad("document_root needs a record class")
    shape = {"fields": kind == "struct", "reserved": kind == "struct", "enumerators": kind == "enum",
             "cases": kind == "variant"}
    for key, allowed in shape.items():
        if t.get(key) and not allowed:
            bad("%s is not allowed on kind %s" % (key, kind))
    if kind == "enum" and not t.get("enumerators"):
        bad("an enum needs enumerators")
    if kind == "variant" and not t.get("cases"):
        bad("a variant needs cases")
    needs_element = kind in ("list", "map", "oset", "tagged")
    if needs_element and "element" not in t:
        bad("kind %s needs an element type" % kind)
    if not needs_element and "element" in t:
        bad("element is not allowed on kind %s" % kind)
    if kind == "map" and "key" not in t:
        bad("a map needs a key type")
    if kind != "map" and "key" in t:
        bad("key is only allowed on a map")
    for key in ("element", "key"):
        if key in t and _resolve(known, t[key]) is None:
            bad("%s type %s is not a scalar or a known type" % (key, t[key]))
    if "key" in t and t["key"] not in ("string", "id") and t["key"] not in INT_RANGES:
        bad("map keys are string, id or integer types, not %s" % t["key"])
    _check_fields(rel, t, known, subject, errors)
    _check_members(rel, t, known, subject, errors)


def _check_fields(rel, t, known, subject, errors):
    def bad(message):
        errors.append(_err(rel, "R-SCH-WELLFORMED", subject, message))

    fields, reserved = t.get("fields", []), t.get("reserved", [])
    _unique(fields, "no", rel, subject, "field number", errors)
    _unique(fields, "name", rel, subject, "field name", errors)
    for value in sorted({r for r in reserved if reserved.count(r) > 1}):
        bad("reserved number %d is listed twice" % value)
    for f in sorted(fields, key=lambda x: x["no"]):
        label = "field %d (%s)" % (f["no"], f["name"])
        if f["no"] in reserved:
            errors.append(_err(rel, "R-SCH-REUSE", subject, "%s uses a reserved number" % label))
        resolved = _resolve(known, f["type"], allow_ref=True)
        if resolved is None:
            bad("%s has unknown type %s" % (label, f["type"]))
        if (f["type"] == schemalib.REF_TYPE) != ("ref" in f):
            bad("%s: `ref` is required exactly when the type is ref" % label)
        for target in f.get("ref", {}).get("targets", []):
            target_type = known.get(target)
            if target_type is None or target_type["record_class"] == "none":
                bad("%s ref target %s is not a record type" % (label, target))
        if "deprecated_in" in f and f["deprecated_in"] < f.get("since_minor", 0):
            bad("%s deprecated_in %d is below since_minor %d" % (label, f["deprecated_in"], f["since_minor"]))
        if "default" in f:
            _check_default(f, label, resolved, bad)


def _check_default(f, label, resolved, bad):
    if f["presence"] != "optional":
        bad("%s: only an optional field may declare a default" % label)
    elif resolved is None:
        return
    elif f["type"] in schemalib.DEFAULT_SCALARS:
        try:
            schemalib._default_value(f["type"], f["default"])
        except (schemalib.SchemaLibError, ValueError) as exc:
            bad("%s: %s" % (label, exc))
    elif isinstance(resolved, dict) and resolved["kind"] == "enum":
        numbers = [e["no"] for e in resolved.get("enumerators", [])]
        if f["default"] not in numbers or isinstance(f["default"], bool):
            bad("%s: default %r is not an enumerator number of %s" % (label, f["default"], f["type"]))
    else:
        bad("%s: a default is allowed for bool, integer, float, string and enum fields only" % label)


def _check_members(rel, t, known, subject, errors):
    def bad(message):
        errors.append(_err(rel, "R-SCH-WELLFORMED", subject, message))

    for key, label in (("enumerators", "enumerator"), ("cases", "case")):
        members = t.get(key, [])
        _unique(members, "no", rel, subject, label + " number", errors)
        _unique(members, "name", rel, subject, label + " name", errors)
        for m in members:
            if key == "cases" and _resolve(known, m["type"]) is None:
                bad("case %d (%s) has unknown type %s" % (m["no"], m["name"], m["type"]))
            if "deprecated_in" in m and m["deprecated_in"] < m.get("since_minor", 0):
                bad("%s %d deprecated_in is below since_minor" % (label, m["no"]))


def _edges(t, known):
    """Types that a value of `t` must contain: required struct fields, variant cases, the tagged base."""
    out = []
    if t["kind"] == "struct":
        out = [f["type"] for f in t.get("fields", []) if f["presence"] == "required"]
    elif t["kind"] == "variant":
        out = [c["type"] for c in t.get("cases", [])]
    elif t["kind"] == "tagged":
        out = [t["element"]] if "element" in t else []
    return [name for name in out if name in known]


def _recursion(by_file, known, errors):
    done = set()
    for name in sorted(known):
        path, stack = [], [(name, iter(_edges(known[name], known)))]
        path.append(name)
        seen = {name}
        while stack:
            node, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                stack.pop()
                path.pop()
                continue
            if nxt == name and name not in done:
                cycle = path + [name]
                if name == min(cycle):
                    errors.append(_err(by_file[name], "R-SCH-WELLFORMED", name,
                                       "recurses without an optional field, list, map or oset: "
                                       + " -> ".join(cycle)))
                done.add(name)
            elif nxt not in seen:
                seen.add(nxt)
                path.append(nxt)
                stack.append((nxt, iter(_edges(known[nxt], known))))


def _ownership(modules, arch, errors):
    registry, claims = arch["registry"], {}
    for rel, module in modules:
        domain, owner = module["domain"], module["owner"]
        prefix = domain.split(".")[0]
        claims.setdefault(domain, []).append((rel, owner))
        if prefix not in registry:
            errors.append(_err(rel, "R-OWN-ORPHAN", module["module"], "domain %s is not under a reserved prefix (%s)"
                               % (domain, ", ".join(sorted(registry)))))
            continue
        if registry[prefix]["owner"] != owner:
            errors.append(_err(rel, "R-OWN-DUP", module["module"], "domain %s claimed by owner %s (%s); the "
                               "domain_registry gives prefix %s to owner %s" % (domain, owner, rel, prefix,
                                                                                registry[prefix]["owner"])))
        if prefix == "fabric" and ".".join(domain.split(".")[:2]) not in arch["fabric_domains"]:
            errors.append(_err(rel, "R-OWN-ORPHAN", module["module"], "domain %s is not one of %s"
                               % (domain, ", ".join(arch["fabric_domains"]))))
        for t in module["types"]:
            tdomain = t.get("domain") or domain
            if tdomain != domain and not tdomain.startswith(domain + "."):
                errors.append(_err(rel, "R-OWN-ORPHAN", "%s %s" % (t["name"], t["version"]),
                                   "domain %s is outside its module domain %s" % (tdomain, domain)))
            allowed = registry[prefix]["state_classes"]
            if t["state_class"] in schemalib.STATE_CLASSES and t["state_class"] not in allowed:
                errors.append(_err(rel, "R-OWN-STATE", "%s %s" % (t["name"], t["version"]),
                                   "state class %s is not admitted under prefix %s (%s)"
                                   % (t["state_class"], prefix, ", ".join(allowed))))
    for domain in sorted(claims):
        owners = sorted({o for _rel, o in claims[domain]})
        if len(owners) > 1:
            claimants = "; ".join("%s owner %s" % (rel, owner) for rel, owner in sorted(claims[domain]))
            errors.append("%s: R-OWN-DUP: domain %s has %d owners: %s"
                          % (sorted(claims[domain])[0][0], domain, len(owners), claimants))


# ---- lock rules ------------------------------------------------------------------------------------------------

def _latest(versions):
    return max(versions, key=schemalib.split_version)


def _lock_rules(modules, lock, errors):
    lock_rel = "%s/schema.lock.json" % DIR_REL
    current = {}
    for rel, module in modules:
        for t in module["types"]:
            current[t["name"]] = (rel, module, t)
    for name in sorted(lock["released"]):
        if name not in current:
            errors.append(_err(lock_rel, "R-SCH-RELEASED", name, "released types may not leave the sources "
                               "(versions %s are in the lock)" % ", ".join(sorted(lock["released"][name]))))
    for name in sorted(current):
        rel, module, t = current[name]
        released = lock["released"].get(name)
        if not released:
            continue
        subject = "%s %s" % (name, t["version"])
        n = schemalib.normalize(t, module["domain"])
        try:
            digest = schemalib.lock_hash(n)
        except schemalib.SchemaLibError:
            continue  # already reported as R-SCH-WELLFORMED where it is a defect
        latest = _latest(released)
        if t["version"] in released:
            if released[t["version"]]["hash"] != digest:
                errors.append(_err(rel, "R-SCH-RELEASED", subject, "is released and its definition changed "
                                   "(lock %s, now %s); bump the version instead of editing it"
                                   % (released[t["version"]]["hash"], digest)))
        elif schemalib.split_version(t["version"]) < schemalib.split_version(latest):
            errors.append(_err(rel, "R-SCH-RELEASED", subject, "is below the released version %s" % latest))
        else:
            _bump_rules(rel, name, t, latest, released[latest]["definition"], n, errors)


def _bump_rules(rel, name, t, latest, old, new, errors):
    subject = "%s %s" % (name, t["version"])
    found = schema_compat.changes(old, new)
    forbidden = [c for c in found if c.cls == "forbidden"]
    for c in forbidden:
        errors.append(_err(rel, RULE_OF_CODE.get(c.code, "R-SCH-FORBIDDEN"), subject,
                           "%s since %s: %s" % (c.code, latest, c.detail)))
    if forbidden:
        return
    cls = found[0].cls if found else "identical"
    required, migration = schema_compat.required_bump(cls)
    if required == "none":
        required = "minor"  # any edit changes the object id, so it needs a new (type, version)
    given = schema_compat.given_bump(latest, t["version"])
    if not schema_compat.bump_satisfies(given, required):
        why = found[0].detail if found else "definition unchanged"
        errors.append(_err(rel, "R-SCH-BUMP", subject, "%s change since %s needs a %s bump%s, version goes %s -> %s "
                           "(%s)" % (cls, latest, required, " and a migration step" if migration == "step"
                                     else " and an identity migration step" if migration == "identity" else "",
                                     latest, t["version"], why)))


# ---- driver ----------------------------------------------------------------------------------------------------

def check_state(modules, lock, arch):
    """All semantic errors for already schema-valid modules."""
    errors, known, by_file = [], {}, {}
    for rel, module in modules:
        for t in module["types"]:
            if t["name"] in known:
                errors.append(_err(rel, "R-SCH-NAME", "%s %s" % (t["name"], t["version"]),
                                   "type name is also defined in %s" % by_file[t["name"]]))
            else:
                known[t["name"]] = t
                by_file[t["name"]] = rel
    for rel, module in modules:
        for t in module["types"]:
            if known.get(t["name"]) is t:
                _check_type(rel, module, t, known, arch, errors)
    _recursion(by_file, known, errors)
    _ownership(modules, arch, errors)
    if lock is not None:
        _lock_rules(modules, lock, errors)
    return errors


def check(root):
    """Return (errors, counts) for a repository root; counts is (modules, types, released)."""
    arch, errors = schemalib.load_arch(root)
    modules, more = load_modules(root)
    errors.extend(more)
    lock, more = load_lock(root)
    errors.extend(more)
    if arch is not None:
        errors.extend(check_state(modules, lock if not more else None, arch))
    released = sum(len(v) for v in lock["released"].values()) if lock and not more else 0
    return errors, (len(modules), sum(len(m["types"]) for _rel, m in modules), released)


def lock_entry(t, domain):
    n = schemalib.normalize(t, domain)
    return schemalib.version_text(n), {"hash": schemalib.lock_hash(n), "definition": n}


def lock_new(root):
    """Pin every unreleased (type, version) of the sources; returns (count, errors)."""
    errors, _counts = check(root)
    if errors:
        return 0, errors
    modules, _ = load_modules(root)
    lock, _ = load_lock(root)
    added = 0
    for _rel, module in modules:
        for t in module["types"]:
            version, entry = lock_entry(t, module["domain"])
            slot = lock["released"].setdefault(t["name"], {})
            if version not in slot:
                slot[version] = entry
                added += 1
    for name in lock["released"]:
        lock["released"][name] = {v: lock["released"][name][v]
                                  for v in sorted(lock["released"][name], key=schemalib.split_version)}
    lock["released"] = {n: lock["released"][n] for n in sorted(lock["released"])}
    path = os.path.join(root, *DIR_REL.split("/"), "schema.lock.json")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(lock, indent=2, ensure_ascii=False) + "\n")
    return added, []


def main(argv):
    root, args, command = schemalib.ROOT, argv[1:], "check"
    if args and args[0] in ("check", "lock"):
        command, args = args[0], args[1:]
    if args[:1] == ["--root"] and len(args) == 2:
        root = os.path.abspath(args[1])
    elif args:
        print("usage: schema_check.py [check|lock] [--root DIR]", file=sys.stderr)
        return 2
    if command == "lock":
        added, errors = lock_new(root)
        for e in errors:
            print(e)
        if errors:
            print("fabric-schema-lock: refused (%d error(s))" % len(errors))
            return 1
        print("fabric-schema-lock: pinned %d (type, version) entries" % added)
        return 0
    errors, (modules, types, released) = check(root)
    for e in errors:
        print(e)
    print("fabric-schema-check: %s" % ("FAIL (%d error(s))" % len(errors) if errors else "ok"))
    print("modules %d types %d released %d" % (modules, types, released))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
