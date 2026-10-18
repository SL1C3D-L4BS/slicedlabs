"""registry-schema-selftest: the schemas, the subset validator and the rule fixtures agree.

1. Every schemas/sl-*.schema.json loads and stays inside the supported keyword subset (slib).
2. The inline validator cases give the expected verdict.
3. The good fixture passes every rule; each bad-<rule>.json fails with exactly that rule id;
   every rule id has a fixture.
4. When python `jsonschema` is importable, it must agree with the subset validator on the schemas
   (meta-schema), the validator cases, every fixture document and every generated mutant of the
   good fixture. Without it, the cross-check is reported as skipped. Run with `python3 -I`."""

import copy
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slib  # noqa: E402

FIXTURES = os.path.join(slib.ROOT, "tests", "governance", "fixtures")
SCHEMAS = (
    "sl-reg-001",
    "sl-reg-002",
    "sl-reg-003",
    "sl-reg-004",
    "sl-reg-005",
    "sl-reg-006",
    "sl-doc-frontmatter",
    "sl-wo-trace",
)
# Instances of the good bundle and the schema each one is checked against.
BUNDLE_PARTS = (("registry", "sl-reg-002"), ("documents", "sl-reg-001"))
WRONG_VALUES = (None, True, 0, 1.5, "", "x", [], {})


def mutants(node):
    """Yield (description, mutated copy) for one-step edits of every node of a JSON document."""

    def at(root, path):
        for p in path:
            root = root[p]
        return root

    def walk(root, path):
        node = at(root, path)
        if isinstance(node, dict):
            for key in list(node):
                yield path + [key], "delete", None
                yield path + [key], "replace", None
            yield path, "extra", None
            for key in list(node):
                yield from walk(root, path + [key])
        elif isinstance(node, list):
            for i in range(len(node)):
                yield from walk(root, path + [i])
            if node:
                yield path, "append-first", None
        else:
            yield path, "replace", None

    for path, kind, _ in walk(node, []):
        if kind == "delete":
            m = copy.deepcopy(node)
            del at(m, path[:-1])[path[-1]]
            yield "delete %s" % path, m
        elif kind == "extra":
            m = copy.deepcopy(node)
            at(m, path)["x_extra"] = 1
            yield "extra key at %s" % path, m
        elif kind == "append-first":
            m = copy.deepcopy(node)
            lst = at(m, path)
            lst.append(copy.deepcopy(lst[0]))
            yield "duplicate first item at %s" % path, m
        elif path:
            for wrong in WRONG_VALUES:
                m = copy.deepcopy(node)
                at(m, path[:-1])[path[-1]] = wrong
                yield "set %s to %r" % (path, wrong), m


def main():
    errors = []
    schemas = {}
    for name in SCHEMAS:
        try:
            schemas[name] = slib.load_schema(name)
        except slib.LoadError as exc:
            errors.append(str(exc))
            continue
        for e in slib.check_schema_subset(schemas[name]):
            errors.append("schemas/%s.schema.json: %s" % (name, e))
        if schemas[name].get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            errors.append("schemas/%s.schema.json: $schema must be the 2020-12 meta-schema" % name)
    extra = sorted(
        os.path.basename(p)[: -len(".schema.json")]
        for p in glob.glob(os.path.join(slib.SCHEMA_DIR, "sl-*.schema.json"))
    )
    for name in extra:
        if name not in SCHEMAS and name not in ("sl-deps", "sl-deps-lock", "sl-provenance", "sl-log-event"):
            errors.append("schemas/%s.schema.json: not covered by the selftest" % name)

    cases = slib.load_json(os.path.join(FIXTURES, "validator-cases.json"))["cases"]
    for case in cases:
        sub = slib.check_schema_subset(case["schema"])
        if sub:
            errors.append("case %r: schema outside the subset: %s" % (case["name"], sub[0]))
        elif slib.is_valid(case["instance"], case["schema"]) != case["valid"]:
            errors.append("case %r: subset validator disagrees with the expected verdict" % case["name"])

    good = slib.load_json(os.path.join(FIXTURES, "good-minimal.json"))
    for f in slib.check_bundle(good):
        errors.append("good-minimal.json: unexpected %s" % slib.format_finding(f))
    seen = set()
    for path in sorted(glob.glob(os.path.join(FIXTURES, "bad-*.json"))):
        rule = os.path.basename(path)[len("bad-") : -len(".json")]
        found = {f["rule"] for f in slib.check_bundle(slib.load_json(path))}
        if rule not in slib.RULES:
            errors.append("%s: names an unknown rule" % os.path.basename(path))
        elif found != {rule}:
            errors.append(
                "%s: expected exactly %s, found %s" % (os.path.basename(path), rule, sorted(found) or "nothing")
            )
        seen.add(rule)
    for rule in slib.RULES:
        if rule not in seen:
            errors.append("rule %s has no bad-%s.json fixture" % (rule, rule))

    checked = 0
    try:
        from importlib import metadata  # noqa: PLC0415

        import jsonschema  # noqa: PLC0415
    except ImportError:
        jsonschema = None
    if jsonschema is not None:
        validator = jsonschema.Draft202012Validator
        for name, schema in schemas.items():
            try:
                validator.check_schema(schema)
            except jsonschema.SchemaError as exc:
                errors.append("schemas/%s.schema.json: not a valid 2020-12 schema: %s" % (name, exc.message))

        built = {}

        def agree(label, instance, schema):
            nonlocal checked
            checked += 1
            mine = slib.is_valid(instance, schema)
            if id(schema) not in built:
                built[id(schema)] = (schema, validator(schema))
            theirs = built[id(schema)][1].is_valid(instance)
            if mine != theirs:
                errors.append("%s: subset validator says %s, jsonschema says %s" % (label, mine, theirs))

        for case in cases:
            agree("case %r" % case["name"], case["instance"], case["schema"])
        paths = sorted(glob.glob(os.path.join(FIXTURES, "*.json")))
        for path in paths:
            if os.path.basename(path) == "validator-cases.json":
                continue
            bundle = slib.load_json(path)
            for key, name in BUNDLE_PARTS:
                if key in bundle:
                    agree("%s %s" % (os.path.basename(path), key), bundle[key], schemas[name])
            for i, wo in enumerate(bundle.get("work_orders", [])):
                agree("%s work_orders/%d" % (os.path.basename(path), i), wo, schemas["sl-wo-trace"])
        for key, name in BUNDLE_PARTS:
            for desc, m in mutants(good[key]):
                agree("good-minimal %s: %s" % (key, desc), m, schemas[name])
        for i, wo in enumerate(good["work_orders"]):
            for desc, m in mutants(wo):
                agree("good-minimal work_orders/%d: %s" % (i, desc), m, schemas["sl-wo-trace"])
        front = {
            "id": "SL-GOV-001", "kind": "GOV", "title": "T", "status": "Draft", "version": "0.1.0",
            "owner": "founder", "episodes": ["020"], "depends_on": [], "supersedes": [], "decisions": ["ADR-0001"],
        }
        agree("front matter sample", front, schemas["sl-doc-frontmatter"])
        for desc, m in mutants(front):
            agree("front matter: %s" % desc, m, schemas["sl-doc-frontmatter"])

    if errors:
        for e in errors[:40]:
            print("registry-schema-selftest: FAIL: %s" % e)
        if len(errors) > 40:
            print("registry-schema-selftest: FAIL: ... and %d more" % (len(errors) - 40))
        return 1
    if jsonschema is None:
        note = "jsonschema not importable: cross-check skipped"
    else:
        note = "jsonschema %s agrees on %d instances" % (metadata.version("jsonschema"), checked)
    print(
        "registry-schema-selftest: ok (%d schemas, %d validator cases, %d rules with fixtures; %s)"
        % (len(schemas), len(cases), len(slib.RULES), note)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
