"""Fabric schema library (WO-040, ADR-0013, SL-ARCH-001 Schemas and versioning). Stdlib only (ADR-0004).

Loads and validates `schemas/fabric/*.sltype.json` modules (format sl-type-def/1,
schemas/fabric/sl-type-def.schema.json) and computes the schema object id that the lock pins and that the C++
codec must reproduce byte for byte:

  normalize(type, default_domain) -> dict     the canonical definition: explicit keys, lists sorted by number
  type_def_value(type, default_domain)        the SLB/1 value of the SL-ARCH-001 `type_def_value` layout
  type_def_bytes(...) / lock_oid(...)         SLB/1 bytes; object id (kind 9, major 1, minor 0, flags 0)
  lock_hash(type, default_domain) -> str      "sha256:<64 hex>" of that object id (the schema.lock.json value)

Value encodings that SL-ARCH-001 leaves open (fixed here, documented in schemas/fabric/README.md):
  name, kind, domain, record_class, state_class, presence, ref strength are Str; major, minor, no, since_minor,
  deprecated_in are Uint; document_root is a bool; `reserved` is a List of Uint ascending; fields, enumerators and
  cases are Lists of Structs sorted by number. Optional keys (field 5 default, 7 deprecated_in, 8 ref; 13 element,
  14 key) are absent when unset. A default is the SLB/1 value of the field type (Uint, Nint, bool, F32, F64, Str;
  an enum default is its enumerator number). The type-level `doc` is not part of the value.
  enumerator: Struct {1 no, 2 name, 3 doc, 4 since_minor, 5 deprecated_in optional}
  case:       Struct {1 no, 2 name, 3 type, 4 doc, 5 since_minor, 6 deprecated_in optional}

`python3 -I tools/fabric/schemalib.py vectors check|write` verifies or rewrites
tests/fixtures/fabric/schema/lock_vectors.json (the hash of every fixture type; WO-041/WO-045 check C++ against it).
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "governance"))
import cmd_constitution_check  # noqa: E402
import slb_ref  # noqa: E402
import slib  # noqa: E402
from slb_ref import F32, F64, List, Nint, Str, Struct, Uint  # noqa: E402

SCHEMA_REL = "schemas/fabric/sl-type-def.schema.json"
LOCK_REL = "schemas/fabric/schema.lock.json"
MODULE_SUFFIX = ".sltype.json"
FORMAT = "sl-type-def/1"
KIND_SCHEMA = 9
KINDS = ("struct", "enum", "variant", "list", "map", "oset", "tagged")
STATE_CLASSES = ("document", "derived", "observation")
INT_RANGES = {
    "i8": (-(1 << 7), (1 << 7) - 1), "i16": (-(1 << 15), (1 << 15) - 1),
    "i32": (-(1 << 31), (1 << 31) - 1), "i64": (-(1 << 63), (1 << 63) - 1),
    "u8": (0, (1 << 8) - 1), "u16": (0, (1 << 16) - 1), "u32": (0, (1 << 32) - 1), "u64": (0, (1 << 64) - 1),
}
SCALARS = frozenset(INT_RANGES) | {"bool", "f32", "f64", "string", "bytes", "id", "digest", "blob_ref", "external_ref"}
DEFAULT_SCALARS = frozenset(INT_RANGES) | {"bool", "f32", "f64", "string"}
REF_TYPE = "ref"
STRENGTHS = ("weak", "strong", "owned")
MAX_NUMBER = (1 << 32) - 1


class SchemaLibError(ValueError):
    """A definition that cannot be normalized or encoded (callers turn it into R-SCH-WELLFORMED)."""


# ---- loading ---------------------------------------------------------------------------------------------------

def load_arch(root):
    """Return (arch, errors): the sl-data tables of SL-ARCH-001 that this tooling reads.

    arch keys: id_classes (names), registry {prefix: row}, fabric_domains, compatibility (list of rows), states.
    """
    arch_dir = os.path.join(root, "docs", "arch")
    names = sorted(os.listdir(arch_dir)) if os.path.isdir(arch_dir) else []
    found = [n for n in names if n.startswith("SL-ARCH-001-") and n.endswith(".md")]
    if not found:
        return None, ["docs/arch/SL-ARCH-001-*.md: R-SCH-ARCH: the canonical project model is missing"]
    rel = "docs/arch/" + found[0]
    try:
        with open(os.path.join(arch_dir, found[0]), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return None, ["%s: R-SCH-ARCH: unreadable: %s" % (rel, exc)]
    blocks, bad = cmd_constitution_check.sl_data_blocks(text)
    errors = ["%s: R-SCH-ARCH: %s" % (rel, b) for b in bad]
    merged = {}
    for block in blocks:
        if isinstance(block, dict):
            merged.update(block)
    for key in ("id_classes", "domain_registry", "fabric_domains", "compatibility"):
        if key not in merged:
            errors.append("%s: R-SCH-ARCH: no sl-data block holds `%s`" % (rel, key))
    if errors:
        return None, errors
    arch = {
        "id_classes": [c["name"] for c in merged["id_classes"]],
        "registry": {row["prefix"]: row for row in merged["domain_registry"]},
        "fabric_domains": list(merged["fabric_domains"]),
        "compatibility": list(merged["compatibility"]),
    }
    return arch, []


def schema_path(root):
    return os.path.join(root, *SCHEMA_REL.split("/"))


def load_type_def_schema(root):
    return slib.load_json(schema_path(root))


def module_files(directory):
    """Sorted file names of `*.sltype.json` modules in a directory (empty when it does not exist)."""
    if not os.path.isdir(directory):
        return []
    return sorted(n for n in os.listdir(directory) if n.endswith(MODULE_SUFFIX))


def validate_module(module, schema):
    """JSON Schema findings for one module file: a list of (pointer, message)."""
    return slib.validate(module, schema)


# ---- normal form -----------------------------------------------------------------------------------------------

def split_version(text):
    major, minor = text.split(".")
    return int(major), int(minor)


def _norm_field(f):
    out = {"no": f["no"], "name": f["name"], "type": f["type"], "presence": f["presence"]}
    if "default" in f:
        out["default"] = f["default"]
    out["since_minor"] = f.get("since_minor", 0)
    if "deprecated_in" in f:
        out["deprecated_in"] = f["deprecated_in"]
    if "ref" in f:
        out["ref"] = {"strength": f["ref"]["strength"], "targets": sorted(f["ref"]["targets"])}
    out["doc"] = f.get("doc", "")
    return out


def _norm_member(m, with_type):
    out = {"no": m["no"], "name": m["name"]}
    if with_type:
        out["type"] = m["type"]
    out["doc"] = m.get("doc", "")
    out["since_minor"] = m.get("since_minor", 0)
    if "deprecated_in" in m:
        out["deprecated_in"] = m["deprecated_in"]
    return out


def normalize(t, default_domain=None):
    """The canonical form of a type definition (idempotent: a normalized definition normalizes to itself)."""
    if "major" in t:
        major, minor = t["major"], t["minor"]
    else:
        major, minor = split_version(t["version"])
    out = {
        "name": t["name"], "kind": t["kind"], "major": major, "minor": minor,
        "domain": t.get("domain") or default_domain or "",
        "record_class": t["record_class"], "state_class": t["state_class"],
        "document_root": bool(t.get("document_root", False)),
        "fields": sorted((_norm_field(f) for f in t.get("fields", [])), key=lambda f: f["no"]),
        "reserved": sorted(set(t.get("reserved", []))),
        "enumerators": sorted((_norm_member(e, False) for e in t.get("enumerators", [])), key=lambda e: e["no"]),
        "cases": sorted((_norm_member(c, True) for c in t.get("cases", [])), key=lambda c: c["no"]),
    }
    for key in ("element", "key"):
        if key in t:
            out[key] = t[key]
    return out


def version_text(n):
    return "%d.%d" % (n["major"], n["minor"])


# ---- SLB/1 value -----------------------------------------------------------------------------------------------

def _default_value(type_name, value):
    """The SLB/1 value of a field default; SchemaLibError when it does not fit the declared type."""
    if type_name == "bool":
        if not isinstance(value, bool):
            raise SchemaLibError("default of bool must be true or false")
        return value
    if type_name in INT_RANGES:
        low, high = INT_RANGES[type_name]
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise SchemaLibError("default %r is not an integer of %s" % (value, type_name))
        return Uint(value) if value >= 0 else Nint(value)
    if type_name in ("f32", "f64"):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise SchemaLibError("default of %s must be a number" % type_name)
        out = (F32 if type_name == "f32" else F64)(float(value))
        try:
            slb_ref.encode(out)
        except slb_ref.SlbError as exc:
            raise SchemaLibError("default %r is not a finite %s: %s" % (value, type_name, exc)) from exc
        return out
    if type_name == "string":
        if not isinstance(value, str):
            raise SchemaLibError("default of string must be a string")
        return Str(value)
    # an enum default is the enumerator number
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= MAX_NUMBER:
        raise SchemaLibError("default of %s must be an enumerator number" % type_name)
    return Uint(value)


def _field_value(f):
    items = [(1, Uint(f["no"])), (2, Str(f["name"])), (3, Str(f["type"])), (4, Str(f["presence"]))]
    if "default" in f:
        items.append((5, _default_value(f["type"], f["default"])))
    items.append((6, Uint(f["since_minor"])))
    if "deprecated_in" in f:
        items.append((7, Uint(f["deprecated_in"])))
    if "ref" in f:
        items.append((8, Struct(((1, Str(f["ref"]["strength"])),
                                 (2, List(tuple(Str(t) for t in f["ref"]["targets"])))))))
    items.append((9, Str(f["doc"])))
    return Struct(tuple(items))


def _member_value(m, with_type):
    if with_type:
        items = [(1, Uint(m["no"])), (2, Str(m["name"])), (3, Str(m["type"])), (4, Str(m["doc"])),
                 (5, Uint(m["since_minor"]))]
        last = 6
    else:
        items = [(1, Uint(m["no"])), (2, Str(m["name"])), (3, Str(m["doc"])), (4, Uint(m["since_minor"]))]
        last = 5
    if "deprecated_in" in m:
        items.append((last, Uint(m["deprecated_in"])))
    return Struct(tuple(items))


def type_def_value(t, default_domain=None):
    """The `type_def_value` of SL-ARCH-001 as an SLB/1 value."""
    n = normalize(t, default_domain)
    items = [
        (1, Str(n["name"])), (2, Str(n["kind"])), (3, Uint(n["major"])), (4, Uint(n["minor"])),
        (5, Str(n["domain"])), (6, Str(n["record_class"])), (7, Str(n["state_class"])), (8, n["document_root"]),
        (9, List(tuple(_field_value(f) for f in n["fields"]))),
        (10, List(tuple(Uint(r) for r in n["reserved"]))),
        (11, List(tuple(_member_value(e, False) for e in n["enumerators"]))),
        (12, List(tuple(_member_value(c, True) for c in n["cases"]))),
    ]
    if "element" in n:
        items.append((13, Str(n["element"])))
    if "key" in n:
        items.append((14, Str(n["key"])))
    return Struct(tuple(items))


def type_def_bytes(t, default_domain=None):
    try:
        return slb_ref.encode(type_def_value(t, default_domain))
    except slb_ref.SlbError as exc:
        raise SchemaLibError(str(exc)) from exc


def lock_oid(t, default_domain=None):
    """Object id (33 bytes) of the schema object: kind 9, major 1, minor 0, flags 0, SLB/1 of type_def_value."""
    return slb_ref.oid(KIND_SCHEMA, type_def_bytes(t, default_domain), 1, 0, 0)


def lock_hash(t, default_domain=None):
    """`sha256:<hex>`: the value schema.lock.json pins for a released (type, version)."""
    return slb_ref.oid_text(lock_oid(t, default_domain))


# ---- fixture vectors -------------------------------------------------------------------------------------------

VECTORS_REL = "tests/fixtures/fabric/schema/lock_vectors.json"
FIXTURE_MODULES_REL = "tests/fixtures/fabric/schema/modules"


def fixture_vectors(root):
    """Return (document, errors): hash vectors of every type of the fixture modules."""
    directory = os.path.join(root, *FIXTURE_MODULES_REL.split("/"))
    rows, errors = [], []
    for name in module_files(directory):
        try:
            module = slib.load_json(os.path.join(directory, name))
        except slib.LoadError as exc:
            errors.append(str(exc))
            continue
        for t in module["types"]:
            try:
                data = type_def_bytes(t, module["domain"])
                rows.append({"type": t["name"], "version": t["version"], "hex": data.hex(),
                             "hash": lock_hash(t, module["domain"])})
            except SchemaLibError as exc:
                errors.append("%s %s: %s" % (name, t["name"], exc))
    rows.sort(key=lambda r: r["type"])
    return {"format": "sl-lock-vectors/1", "vectors": rows}, errors


def render_vectors(doc):
    rows = ",\n".join(json.dumps(v, ensure_ascii=False) for v in doc["vectors"])
    return '{\n"format": "%s",\n"vectors": [\n%s\n]\n}\n' % (doc["format"], rows)


def main(argv):
    if len(argv) == 3 and argv[1] == "vectors" and argv[2] in ("check", "write"):
        doc, errors = fixture_vectors(ROOT)
        if errors:
            print("\n".join(errors))
            return 1
        path = os.path.join(ROOT, *VECTORS_REL.split("/"))
        text = render_vectors(doc)
        if argv[2] == "write":
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            print("schemalib: wrote %d vectors" % len(doc["vectors"]))
            return 0
        try:
            with open(path, encoding="utf-8") as fh:
                current = fh.read()
        except OSError:
            current = None
        if current != text:
            print("%s: R-SCH-VECTORS: differs from the fixture modules (run `schemalib.py vectors write`)"
                  % VECTORS_REL)
            return 1
        print("schemalib: %d vectors ok" % len(doc["vectors"]))
        return 0
    print("usage: schemalib.py vectors check|write", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
