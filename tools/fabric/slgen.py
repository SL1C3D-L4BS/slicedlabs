"""slgen: schema-generated C++, inspector metadata and text schemas (WO-045, SL-ARCH-001 ARCH1-R67 to ARCH1-R69,
ADR-0013). Stdlib only, no subprocess (ADR-0004); deterministic: the output depends only on the module files.

From the module files `schemas/fabric/*.sltype.json` (format sl-type-def/1, WO-040) it writes, under
engine/fabric/gen/:

  include/sl/fabric/gen/<module>.gen.hpp   typed structs (enum class, struct, std::variant holder, aliases for list,
                                           map, oset and tagged), <type>_to_value / <type>_from_value returning
                                           sl::result, register_types() and types()
  src/<module>.gen.cpp                     descriptor tables (type_desc per type) registered into a schema_registry,
                                           and the conversions: the validator tables of from_value (kind, range,
                                           enumerator, case, required and undeclared field checks)
  include/sl/fabric/gen/support.gen.hpp,   the shared conversion helpers and the oset_item, external_ref and
  src/support.gen.cpp                      type_entry types (static text)
  include/sl/fabric/gen/system.gen.hpp,    register_system_types(), system_registry() (frozen) and system_types()
  src/system.gen.cpp                       over every module
  meta/<module>.meta.json                  inspector metadata: every type and field with doc, domain, owner, state
                                           class, ref targets and the lock hash
  text/<module>.text.schema.json           JSON Schema 2020-12 of the canonical text form of every type of the module
                                           and the types it reaches (FAB-INV-GEN-05, the cut line)

and tests/fixtures/fabric/types/lock_hashes.json (schemalib.lock_hash of every type, read by FAB-INV-GEN-03).

C++ output is clang-format clean by construction: every line stays within 100 columns (a name too long for that is
R-GEN-LONG), short functions are joined onto one line as clang-format does, and long string literals are split at
spaces the way clang-format splits them.

Mapping notes. sl-type-def/1 gives every type a state class, while the C++ type system (type.hpp) treats a state class
as the mark of a record type; descriptors therefore keep the state class only on record types (record_class other than
none) and the type table carries the declared one, which lock_hash() restores so the C++ schema object id equals the
Python lock hash. Enumerators and cases with a since_minor or deprecated_in are refused (R-GEN-UNSUPPORTED): the C++
descriptors cannot carry them (WO-041). Types that reach themselves by value are refused (R-GEN-UNSUPPORTED), and so is
a list whose element is an identity-bearing struct (R-GEN-LIST-ID: the C++ registry refuses it, ARCH1-R15).

Rules of `check` (each line `<path>: <RULE>: message`):
  R-GEN-INPUT    the module set does not pass the sl-type-def/1 schema or the fabric-schema-check module rules
  R-GEN-STALE    a committed generated file differs from the generator output (first differing line named)
  R-GEN-MISSING  a generated file is not on disk
  R-GEN-EXTRA    a file in a generated directory that the generator does not write
  R-GEN-LONG     a generated C++ line would exceed 100 columns (shorten the name or doc word)
  R-GEN-NAME     two generated C++ names collide
  R-GEN-UNSUPPORTED, R-GEN-LIST-ID   see above
  R-GEN-DOC      a type, field, enumerator or case of schemas/fabric has no doc (inspector metadata needs it)
  R-GEN-FIXTURE  a type has no tests/fixtures/fabric/types/<module>/<local>.json with at least two values, a value is
                 not canonical text of its type (checked with tools/fabric/slb_text.py), or a fixture names no type

Run as `python3 -I tools/fabric/slgen.py generate|check [--root DIR]`; check ends with `modules M types T files F`.
"""

import json
import os
import re
import struct
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "governance"))
import schema_check  # noqa: E402
import schemalib  # noqa: E402
import slb_text  # noqa: E402
import slib  # noqa: E402
from slb_ref import SlbError  # noqa: E402

MODULES_REL = "schemas/fabric"
GEN_REL = "engine/fabric/gen"
FIXTURES_REL = "tests/fixtures/fabric/types"
LOCK_HASHES = "lock_hashes.json"
FIXTURE_FORMAT = "sl-type-fixtures/1"
META_FORMAT = "sl-gen-meta/1"
COLUMNS = 100
GEN_DIRS = ("include/sl/fabric/gen", "src", "meta", "text")
RESERVED_MODULES = ("support", "system")
HEADER_DIR = "include/sl/fabric/gen"

INT_CPP = {"i8": "std::int8_t", "i16": "std::int16_t", "i32": "std::int32_t", "i64": "std::int64_t",
           "u8": "std::uint8_t", "u16": "std::uint16_t", "u32": "std::uint32_t", "u64": "std::uint64_t"}
SCALAR_CPP = dict(INT_CPP, **{
    "bool": "bool", "f32": "float", "f64": "double", "string": "std::string", "bytes": "std::vector<std::uint8_t>",
    "id": "fabric::record_id", "ref": "fabric::record_id", "digest": "fabric::digest",
    "blob_ref": "fabric::digest", "external_ref": "gen::external_ref"})
SCALAR_CODEC = {"bool": "bool", "f32": "f32", "f64": "f64", "string": "string", "bytes": "bytes", "id": "id",
                "ref": "id", "digest": "digest", "blob_ref": "digest", "external_ref": "external_ref"}
KIND_CPP = {"struct": "struct_", "enum": "enum_", "variant": "variant", "list": "list", "map": "map",
            "oset": "oset", "tagged": "tagged"}
CPP_KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl
concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do
double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace
new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return
short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef
typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq final override import module
errno assert
""".split())
# Member names a generated struct reserves for itself.
MEMBER_RESERVED = frozenset(["to_value", "from_value", "in", "alt"])
ID_PREFIXES = [(1, "prj"), (2, "sch"), (3, "typ"), (4, "ast"), (5, "ent"), (6, "ins"), (7, "drv"), (8, "txn"),
               (9, "evt"), (10, "act"), (11, "pkg"), (12, "elm"), (13, "nsp"), (14, "ope"), (15, "imp")]
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
MAX_SAFE = (1 << 53) - 1


class GenError(Exception):
    """A module set the generator refuses; `findings` holds `<path>: <RULE>: message` lines."""

    def __init__(self, findings):
        super().__init__("\n".join(findings))
        self.findings = findings


# ---- loading ---------------------------------------------------------------------------------------------------

def load_set(root, modules_dir, rel_dir):
    """Return the module list [(relative path, module)] of a directory after the WO-040 checks; GenError otherwise."""
    findings = []
    try:
        schema = schemalib.load_type_def_schema(root)
    except slib.LoadError as exc:
        raise GenError(["%s: R-GEN-INPUT: %s" % (schemalib.SCHEMA_REL, exc)]) from exc
    modules = []
    for name in schemalib.module_files(modules_dir):
        rel = "%s/%s" % (rel_dir, name)
        try:
            module = slib.load_json(os.path.join(modules_dir, name))
        except slib.LoadError as exc:
            findings.append("%s: R-GEN-INPUT: %s" % (rel, exc))
            continue
        found = schemalib.validate_module(module, schema)
        findings.extend("%s: R-GEN-INPUT: %s: %s" % (rel, p, m) for p, m in found)
        if not found:
            modules.append((rel, module))
    arch, errors = schemalib.load_arch(root)
    findings.extend(errors)
    if arch is not None and not findings:
        findings.extend("%s (R-GEN-INPUT)" % e for e in schema_check.check_state(modules, None, arch))
    if findings:
        raise GenError(findings)
    modules.sort(key=lambda m: m[1]["module"])
    return modules, arch


class Model:
    """The module set with names resolved: types by name, their module, and C++ spellings."""

    def __init__(self, modules, arch):
        self.modules = [m for _rel, m in modules]
        self.rel = {m["module"]: rel for rel, m in modules}
        self.arch = arch
        self.types, self.module_of = {}, {}
        for m in self.modules:
            for t in m["types"]:
                self.types[t["name"]] = t
                self.module_of[t["name"]] = m
        self.findings = []
        self._check_names()

    def local(self, name):
        return name[len(self.module_of[name]["module"]) + 1:]

    def cpp_local(self, name):
        return escape(self.local(name).replace(".", "_"))

    def domain(self, name):
        t = self.types[name]
        return t.get("domain") or self.module_of[name]["domain"]

    def owner(self, domain):
        return self.arch["registry"][domain.split(".")[0]]["owner"]

    def _check_names(self):
        for m in self.modules:
            rel = self.rel[m["module"]]
            if m["module"] in RESERVED_MODULES:
                self.findings.append("%s: R-GEN-NAME: module name %s is reserved" % (rel, m["module"]))
            seen = {"register_types": "a function", "types": "a function"}
            for t in m["types"]:
                local = self.cpp_local(t["name"])
                names = [local, local + "_to_value", local + "_from_value"]
                for n in names:
                    if n in seen:
                        self.findings.append("%s: R-GEN-NAME: %s: C++ name %s is already %s"
                                             % (rel, t["name"], n, seen[n]))
                    seen[n] = "used by " + t["name"]
                members = set()
                for f in t.get("fields", []):
                    member = member_name(f["name"], local)
                    if member in members:
                        self.findings.append("%s: R-GEN-NAME: %s: field %s collides after escaping as %s"
                                             % (rel, t["name"], f["name"], member))
                    members.add(member)

    def deps(self, name):
        """Named types a value of `name` holds by value (struct fields, cases, element, key, tagged base)."""
        t = self.types[name]
        refs = [f["type"] for f in t.get("fields", [])] + [c["type"] for c in t.get("cases", [])]
        refs += [t[k] for k in ("element", "key") if k in t]
        return [r for r in refs if r in self.types]

    def module_order(self):
        """Modules sorted so that a module comes after every module it depends on; GenError on a cycle."""
        graph = {}
        for m in self.modules:
            graph[m["module"]] = sorted({self.module_of[d]["module"] for t in m["types"] for d in self.deps(t["name"])
                                         if self.module_of[d]["module"] != m["module"]})
        return _toposort(graph, lambda cycle: "%s: R-GEN-UNSUPPORTED: modules depend on each other: %s"
                         % (self.rel[cycle[0]], " -> ".join(cycle)))

    def type_order(self, m):
        graph = {}
        for t in m["types"]:
            graph[t["name"]] = sorted(d for d in self.deps(t["name"]) if self.module_of[d] is m)
        return _toposort(graph, lambda cycle: "%s: R-GEN-UNSUPPORTED: %s reaches itself by value: %s"
                         % (self.rel[m["module"]], cycle[0], " -> ".join(cycle)), keep=[t["name"] for t in m["types"]])

    def cpp_type(self, ref):
        if ref in SCALAR_CPP:
            return SCALAR_CPP[ref]
        return "%s::%s" % (ns_of(self.module_of[ref]["module"]), self.cpp_local(ref))

    def codec(self, ref, direction, here):
        """The C++ callable that converts `ref` (direction "enc" or "dec"), spelled from module `here`."""
        if ref in INT_CPP:
            return "detail::%s_int<%s>" % (direction, INT_CPP[ref])
        if ref in SCALAR_CODEC:
            return "detail::%s_%s" % (direction, SCALAR_CODEC[ref])
        suffix = "to_value" if direction == "enc" else "from_value"
        owner = self.module_of[ref]["module"]
        prefix = "" if owner == here else ns_of(owner) + "::"
        return "%s%s_%s" % (prefix, self.cpp_local(ref), suffix)

    def is_scalar(self, ref):
        """True when the C++ type of `ref` is a built-in arithmetic or enum type (it needs a `{}` initializer)."""
        ref = self._base(ref)
        if ref in self.types:
            return self.types[ref]["kind"] == "enum"
        return ref in INT_CPP or ref in ("bool", "f32", "f64")

    def trivially_copyable(self, ref):
        """True when the C++ type of `ref` is trivially copyable (std::move of it is a no-op tidy flags)."""
        if ref in SCALAR_CPP:
            return ref not in ("string", "bytes", "external_ref")
        t = self.types[ref]
        if t["kind"] in ("enum", "tagged"):
            return t["kind"] == "enum" or self.trivially_copyable(t["element"])
        if t["kind"] == "struct":
            return all(self.trivially_copyable(f["type"]) for f in t.get("fields", []))
        if t["kind"] == "variant":
            return all(self.trivially_copyable(c["type"]) for c in t["cases"])
        return False

    def identity_bearing(self, ref):
        t = self.types.get(ref)
        if t is None or t["kind"] != "struct":
            return False
        if t["record_class"] != "none":
            return True
        return any(self._base(f["type"]) == "id" for f in t.get("fields", []))

    def _base(self, ref):
        seen = set()
        while ref in self.types and self.types[ref]["kind"] == "tagged" and ref not in seen:
            seen.add(ref)
            ref = self.types[ref]["element"]
        return ref


def _toposort(graph, describe, keep=None):
    order, state = [], {}
    nodes = keep if keep is not None else sorted(graph)

    def visit(node, path):
        if state.get(node) == "done":
            return
        if state.get(node) == "open":
            cycle = path[path.index(node):] + [node]
            raise GenError([describe(cycle)])
        state[node] = "open"
        for nxt in graph[node]:
            visit(nxt, path + [node])
        state[node] = "done"
        order.append(node)

    for node in nodes:
        visit(node, [])
    return order


def ns_of(module):
    return module.replace(".", "_")


def escape(name):
    return name + "_" if name in CPP_KEYWORDS else name


def member_name(name, struct_name):
    out = name + "_" if name in CPP_KEYWORDS or name in MEMBER_RESERVED else name
    while out == struct_name:
        out += "_"
    return out


# ---- C++ text helpers --------------------------------------------------------------------------------------------

def c_string(text):
    out = []
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


def assign_string(indent, lhs, text):
    """`<lhs> = "<text>";` split at spaces like clang-format splits a string literal over 100 columns."""
    lead = "%s%s = " % (indent, lhs)
    body = c_string(text)[1:-1]
    if len(lead) + len(body) + 3 <= COLUMNS:
        return ['%s"%s";' % (lead, body)]
    lines, col = [], len(lead)
    pad = " " * col
    while len(body) + 3 + col > COLUMNS:
        limit = COLUMNS - col - 2
        cut = body.rfind(" ", 0, limit)
        if cut <= 0:
            raise GenError(["R-GEN-LONG: string %s has no space to split at within %d columns" % (text[:40], COLUMNS)])
        lines.append('%s"%s"' % (lead if not lines else pad, body[:cut + 1]))
        body = body[cut + 1:]
    lines.append('%s"%s";' % (lead if not lines else pad, body))
    return lines


def comment(indent, text, prefix="///"):
    if not text:
        return []
    width = COLUMNS - len(indent) - len(prefix) - 1
    return ["%s%s %s" % (indent, prefix, line) for line in textwrap.wrap(text, width, break_long_words=False,
                                                                       break_on_hyphens=False)]


def function(signature, body):
    """A function definition; one statement that fits goes on the signature line (AllowShortFunctions: All)."""
    if len(body) == 1 and len(signature) + len(body[0].strip()) + 5 <= COLUMNS:
        return ["%s { %s }" % (signature, body[0].strip())]
    return ["%s {" % signature] + body + ["}"]


def if_return(cond, expr, indent="  "):
    return ["%sif (%s) {" % (indent, cond), "%s  return %s;" % (indent, expr), "%s}" % indent]


def banner(source):
    return ["// Generated by tools/fabric/slgen.py (WO-045) from %s." % source,
            "// Do not edit: `python3 -I tools/fabric/slgen.py generate` rewrites it and `just fabric-gen-check`",
            "// fails when it drifts (SL-ARCH-001 ARCH1-R67, ARCH1-R68, ADR-0013)."]


def includes(names, quoted):
    return ["#include %s" % (('"%s"' if quoted else "<%s>") % n) for n in sorted(names)]


# ---- module header -------------------------------------------------------------------------------------------------

def render_header(model, m):
    mod = m["module"]
    ns = ns_of(mod)
    deps = sorted({model.module_of[d]["module"] for t in m["types"] for d in model.deps(t["name"])} - {mod})
    lines = ["#pragma once", ""] + banner("schemas/fabric/%s.sltype.json" % mod) + ["//"]
    lines += comment("", "Module %s (domain %s, owner %s). %s" % (mod, m["domain"], m["owner"], m.get("doc", "")),
                     "//")
    lines += [""]
    lines += includes(["cstdint", "map", "optional", "string", "variant", "vector"], False) + [""]
    sl_headers = ["sl/core/error.hpp", "sl/fabric/digest.hpp", "sl/fabric/gen/support.gen.hpp", "sl/fabric/id.hpp",
                  "sl/fabric/schema.hpp", "sl/fabric/value.hpp"]
    sl_headers += ["sl/fabric/gen/%s.gen.hpp" % d for d in deps]
    lines += includes(sl_headers, True) + ["", "namespace sl::fabric::gen::%s {" % ns, ""]
    for name in model.type_order(m):
        lines += _header_type(model, m, model.types[name]) + [""]
    lines += comment("", "Adds the descriptors of every type of %s to `reg` (the registry checks each one)." % mod)
    lines += ["[[nodiscard]] sl::result<void> register_types(fabric::schema_registry &reg);", ""]
    lines += comment("", "The type table of %s, in declaration order." % mod)
    lines += ["[[nodiscard]] std::vector<gen::type_entry> types();", ""]
    lines += ["} // namespace sl::fabric::gen::%s" % ns]
    return lines


def _version(t):
    return t["version"]


def _header_type(model, m, t):
    local = model.cpp_local(t["name"])
    kind = t["kind"]
    doc = "%s %s %s: %s" % (t["name"], kind, _version(t), t.get("doc", "") or "(no doc)")
    out = comment("", doc)
    if kind == "struct":
        out.append("struct %s {" % local)
        for f in sorted(t.get("fields", []), key=lambda x: x["no"]):
            fdoc = "%d %s (%s, %s)%s" % (f["no"], f["name"], f["type"], f["presence"],
                                         ": " + f["doc"] if f.get("doc") else "")
            out += comment("  ", fdoc)
            ctype = model.cpp_type(f["type"])
            if f["presence"] == "optional":
                ctype = "std::optional<%s>" % ctype
            init = "{}" if f["presence"] == "required" and model.is_scalar(f["type"]) else ""
            out.append("  %s %s%s;" % (ctype, member_name(f["name"], local), init))
        if t.get("fields"):
            out.append("")
        out += _members(local)
        out.append("};")
    elif kind == "variant":
        out.append("struct %s {" % local)
        cases = sorted(t["cases"], key=lambda c: c["no"])
        out += comment("  ", "Alternatives in case-number order: %s." % ", ".join(
            "%d %s (%s)" % (c["no"], c["name"], c["type"]) for c in cases))
        out.append("  std::variant<%s> alt;" % ", ".join(model.cpp_type(c["type"]) for c in cases))
        out.append("")
        out += _members(local)
        out.append("};")
    elif kind == "enum":
        out.append("enum class %s : std::uint32_t {" % local)
        for e in sorted(t["enumerators"], key=lambda x: x["no"]):
            out += comment("  ", e.get("doc", ""))
            out.append("  %s = %d," % (escape(e["name"]), e["no"]))
        out.append("};")
    else:
        out.append("using %s = %s;" % (local, _alias(model, t)))
    out += ["", "[[nodiscard]] sl::result<fabric::value> %s_to_value(const %s &in);" % (local, local),
            "[[nodiscard]] sl::result<%s> %s_from_value(const fabric::value &in);" % (local, local)]
    return out


def _alias(model, t):
    kind = t["kind"]
    element = model.cpp_type(t["element"])
    if kind == "list":
        return "std::vector<%s>" % element
    if kind == "oset":
        return "std::vector<gen::oset_item<%s>>" % element
    if kind == "map":
        return "std::map<%s, %s>" % (model.cpp_type(t["key"]), element)
    return element  # tagged: a named newtype of its base


def _members(local):
    return ["  [[nodiscard]] sl::result<fabric::value> to_value() const;",
            "  [[nodiscard]] static sl::result<%s> from_value(const fabric::value &in);" % local,
            "",
            "  friend bool operator==(const %s &, const %s &) = default;" % (local, local)]


# ---- module source -------------------------------------------------------------------------------------------------

def render_source(model, m):
    mod = m["module"]
    ns = ns_of(mod)
    order = model.type_order(m)
    lines = banner("schemas/fabric/%s.sltype.json" % mod) + [""]
    lines += ['#include "sl/fabric/gen/%s.gen.hpp"' % mod, ""]
    lines += includes(["cstdint", "utility", "vector"], False) + [""]
    lines += includes(["sl/core/error.hpp", "sl/fabric/gen/support.gen.hpp", "sl/fabric/schema.hpp",
                       "sl/fabric/type.hpp", "sl/fabric/value.hpp"], True)
    lines += ["", "namespace sl::fabric::gen::%s {" % ns, "", "namespace {", ""]
    for name in order:
        lines += _descriptor(model, model.types[name]) + [""]
    for name in order:
        local = model.cpp_local(name)
        lines += function("sl::result<fabric::value> %s_roundtrip(const fabric::value &in)" % local,
                          ["  return %s_from_value(in).and_then(%s_to_value);" % (local, local)]) + [""]
    lines += ["} // namespace", ""]
    for name in order:
        lines += _conversions(model, m, model.types[name])
    body = ["  std::vector<fabric::type_desc> all;"]
    body += ["  all.push_back(%s_desc());" % model.cpp_local(n) for n in order]
    body += ["  return detail::add_all(reg, std::move(all));"]
    lines += function("sl::result<void> register_types(fabric::schema_registry &reg)", body) + [""]
    body = ["  std::vector<gen::type_entry> out;"]
    for t in m["types"]:
        local = model.cpp_local(t["name"])
        body.append('  out.push_back(detail::entry("%s", %s_roundtrip));' % (t["name"], local))
        body.append("  out.back().declared_state = fabric::state_class::%s;" % t["state_class"])
    body.append("  return out;")
    lines += function("std::vector<gen::type_entry> types()", body) + [""]
    lines += ["} // namespace sl::fabric::gen::%s" % ns]
    return lines


def _default_expr(model, f):
    value = f["default"]
    ftype = f["type"]
    if ftype == "bool":
        return "fabric::value::boolean(%s)" % ("true" if value else "false")
    if ftype in ("f32", "f64"):
        x = float(value)
        if ftype == "f32":
            x = struct.unpack("<f", struct.pack("<f", x))[0]
        text = repr(x)
        return "detail::%s_constant(%s%s)" % (ftype, text, "F" if ftype == "f32" else "")
    if ftype == "string":
        return "detail::string_constant(%s)" % c_string(value)
    if value < 0:
        return "fabric::value::integer(%d)" % value
    return "fabric::value::uint(%d)" % value


def _descriptor(model, t):
    local = model.cpp_local(t["name"])
    major, minor = schemalib.split_version(t["version"])
    record = t["record_class"] != "none"
    body = ['  fabric::type_desc t = detail::make_type("%s");' % t["name"],
            "  t.kind = fabric::type_kind::%s;" % KIND_CPP[t["kind"]],
            "  t.version = {.major = %d, .minor = %d};" % (major, minor)]
    body += assign_string("  ", "t.domain", model.domain(t["name"]))
    if record:
        rc = "namespace_" if t["record_class"] == "namespace" else t["record_class"]
        body.append("  t.record_class = fabric::id_class::%s;" % rc)
        body.append("  t.state_class = fabric::state_class::%s;" % t["state_class"])
    if t.get("document_root"):
        body.append("  t.document_root = true;")
    for f in sorted(t.get("fields", []), key=lambda x: x["no"]):
        body.append('  t.fields.push_back(detail::make_field(%d, "%s"));' % (f["no"], f["name"]))
        body.append('  t.fields.back().type = fabric::type_id_of("%s");' % f["type"])
        if f["presence"] == "optional":
            body.append("  t.fields.back().presence = fabric::presence::optional;")
        if "default" in f:
            body.append("  t.fields.back().default_value = %s;" % _default_expr(model, f))
        if f.get("since_minor", 0):
            body.append("  t.fields.back().since_minor = %d;" % f["since_minor"])
        if "deprecated_in" in f:
            body.append("  t.fields.back().deprecated_in = %d;" % f["deprecated_in"])
        if "ref" in f:
            body.append("  t.fields.back().ref = detail::make_ref(fabric::ref_strength::%s);" % f["ref"]["strength"])
            for target in sorted(f["ref"]["targets"]):
                body.append('  t.fields.back().ref->targets.push_back(fabric::type_id_of("%s"));' % target)
        if f.get("doc"):
            body += assign_string("  ", "t.fields.back().doc", f["doc"])
    if t.get("reserved"):
        body.append("  t.reserved = {%s};" % ", ".join(str(r) for r in sorted(set(t["reserved"]))))
    for e in sorted(t.get("enumerators", []), key=lambda x: x["no"]):
        body.append('  t.enumerators.push_back(detail::make_enumerator(%d, "%s"));' % (e["no"], e["name"]))
        if e.get("doc"):
            body += assign_string("  ", "t.enumerators.back().doc", e["doc"])
    for c in sorted(t.get("cases", []), key=lambda x: x["no"]):
        body.append('  t.cases.push_back(detail::make_case(%d, "%s"));' % (c["no"], c["name"]))
        body.append('  t.cases.back().type = fabric::type_id_of("%s");' % c["type"])
        if c.get("doc"):
            body += assign_string("  ", "t.cases.back().doc", c["doc"])
    for key in ("element", "key"):
        if key in t:
            body.append('  t.%s = fabric::type_id_of("%s");' % (key, t[key]))
    if t.get("doc"):
        body += assign_string("  ", "t.doc", t["doc"])
    body.append("  return t;")
    return function("fabric::type_desc %s_desc()" % local, body)


def _conversions(model, m, t):
    mod = m["module"]
    local = model.cpp_local(t["name"])
    name = t["name"]
    kind = t["kind"]
    enc_sig = "sl::result<fabric::value> %s_to_value(const %s &in)" % (local, local)
    dec_sig = "sl::result<%s> %s_from_value(const fabric::value &in)" % (local, local)
    out = []
    if kind == "struct":
        fields = sorted(t.get("fields", []), key=lambda x: x["no"])
        body = ["  detail::struct_writer w;"]
        for f in fields:
            call = "put_optional" if f["presence"] == "optional" else "put"
            body.append("  w.%s(%d, in.%s, %s);" % (call, f["no"], member_name(f["name"], local),
                                                   model.codec(f["type"], "enc", mod)))
        body.append("  return w.finish();")
        out += function(enc_sig, body) + [""]
        body = ['  detail::struct_reader r(in, "%s");' % name, "  %s out;" % local]
        for f in fields:
            call = "get_optional" if f["presence"] == "optional" else "get"
            body.append("  r.%s(%d, out.%s, %s);" % (call, f["no"], member_name(f["name"], local),
                                                    model.codec(f["type"], "dec", mod)))
        moved = "out" if model.trivially_copyable(name) else "std::move(out)"
        body.append("  return r.finish(%s);" % moved)
        out += function(dec_sig, body) + [""]
        out += _member_defs(local)
    elif kind == "variant":
        cases = sorted(t["cases"], key=lambda c: c["no"])
        body = ["  switch (in.alt.index()) {"]
        for i, c in enumerate(cases):
            body += ["  case %d:" % i, "    return detail::enc_case(%d, std::get<%d>(in.alt), %s);"
                     % (c["no"], i, model.codec(c["type"], "enc", mod))]
        body += ["  default:", '    return detail::valueless("%s");' % name, "  }"]
        out += function(enc_sig, body) + [""]
        body = ['  const auto n = detail::dec_case(in, "%s");' % name] + if_return("!n", "std::unexpected(n.error())")
        body.append("  switch (*n) {")
        for i, c in enumerate(cases):
            body += ["  case %d:" % c["no"], "    return detail::dec_alt<%s, %d>(in.payload(), %s);"
                     % (local, i, model.codec(c["type"], "dec", mod))]
        body += ["  default:", '    return detail::unknown_case("%s", *n);' % name, "  }"]
        out += function(dec_sig, body) + [""]
        out += _member_defs(local)
    elif kind == "enum":
        out += function(enc_sig, ["  return detail::enc_enum(in);"]) + [""]
        body = ['  const auto n = detail::dec_enumerator(in, "%s");' % name]
        body += if_return("!n", "std::unexpected(n.error())") + ["  switch (*n) {"]
        body += ["  case %d:" % e["no"] for e in sorted(t["enumerators"], key=lambda x: x["no"])]
        body += ["    return static_cast<%s>(*n);" % local, "  default:",
                 '    return detail::unknown_enumerator("%s", *n);' % name, "  }"]
        out += function(dec_sig, body) + [""]
    elif kind in ("list", "oset"):
        enc, dec = model.codec(t["element"], "enc", mod), model.codec(t["element"], "dec", mod)
        out += function(enc_sig, ["  return detail::enc_%s(in, %s);" % (kind, enc)]) + [""]
        out += function(dec_sig, ["  return detail::dec_%s(in, %s);" % (kind, dec)]) + [""]
    elif kind == "map":
        ke, kd = model.codec(t["key"], "enc", mod), model.codec(t["key"], "dec", mod)
        ve, vd = model.codec(t["element"], "enc", mod), model.codec(t["element"], "dec", mod)
        out += function(enc_sig, ["  return detail::enc_map(in, %s, %s);" % (ke, ve)]) + [""]
        out += function(dec_sig, ["  return detail::dec_map(in, %s, %s);" % (kd, vd)]) + [""]
    else:  # tagged
        out += function(enc_sig, ["  return %s(in);" % model.codec(t["element"], "enc", mod)]) + [""]
        out += function(dec_sig, ["  return %s(in);" % model.codec(t["element"], "dec", mod)]) + [""]
    return out


def _member_defs(local):
    out = function("sl::result<fabric::value> %s::to_value() const" % local,
                   ["  return %s_to_value(*this);" % local]) + [""]
    out += function("sl::result<%s> %s::from_value(const fabric::value &in)" % (local, local),
                    ["  return %s_from_value(in);" % local]) + [""]
    return out


# ---- system aggregate ------------------------------------------------------------------------------------------------

def render_system_header(model):
    lines = ["#pragma once", ""] + banner("schemas/fabric/*.sltype.json") + ["//"]
    lines += comment("", "Every generated module of the set: %s." % ", ".join(m["module"] for m in model.modules), "//")
    lines += ["", "#include <vector>", ""]
    lines += includes(["sl/core/error.hpp", "sl/fabric/gen/support.gen.hpp", "sl/fabric/schema.hpp"], True)
    lines += ["", "namespace sl::fabric::gen {", ""]
    lines += comment("", "Adds the descriptors of every generated type to `reg`; does not freeze it.")
    lines += ["[[nodiscard]] sl::result<void> register_system_types(fabric::schema_registry &reg);", ""]
    lines += comment("", "A registry holding the built-in and every generated type, frozen.")
    lines += ["[[nodiscard]] sl::result<fabric::schema_registry> system_registry();", ""]
    lines += comment("", "The type table of every generated module, modules in name order.")
    lines += ["[[nodiscard]] std::vector<type_entry> system_types();", "", "} // namespace sl::fabric::gen"]
    return lines


def render_system_source(model):
    lines = banner("schemas/fabric/*.sltype.json") + ["", '#include "sl/fabric/gen/system.gen.hpp"', ""]
    lines += includes(["utility", "vector"], False) + [""]
    sl_headers = ["sl/core/error.hpp", "sl/fabric/gen/support.gen.hpp", "sl/fabric/schema.hpp"]
    sl_headers += ["sl/fabric/gen/%s.gen.hpp" % m["module"] for m in model.modules]
    lines += includes(sl_headers, True) + ["", "namespace sl::fabric::gen {", ""]
    body = []
    for m in model.modules:
        body += if_return("auto r = %s::register_types(reg); !r" % ns_of(m["module"]), "r")
    body.append("  return {};")
    lines += function("sl::result<void> register_system_types(fabric::schema_registry &reg)", body) + [""]
    body = ["  fabric::schema_registry reg;"]
    body += if_return("auto r = register_system_types(reg); !r", "std::unexpected(std::move(r.error()))")
    body += if_return("auto r = reg.freeze(); !r", "std::unexpected(std::move(r.error()))")
    body.append("  return reg;")
    lines += function("sl::result<fabric::schema_registry> system_registry()", body) + [""]
    body = ["  std::vector<type_entry> out;"]
    body += ["  detail::append(out, %s::types());" % ns_of(m["module"]) for m in model.modules]
    body.append("  return out;")
    lines += function("std::vector<type_entry> system_types()", body) + [""]
    lines += ["} // namespace sl::fabric::gen"]
    return lines


# ---- inspector metadata ----------------------------------------------------------------------------------------------

def render_meta(model, m):
    types = []
    for t in m["types"]:
        tdomain = model.domain(t["name"])
        entry = {
            "name": t["name"], "kind": t["kind"], "version": t["version"], "doc": t.get("doc", ""),
            "domain": tdomain, "owner": model.owner(tdomain), "record_class": t["record_class"],
            "state_class": t["state_class"], "document_root": bool(t.get("document_root", False)),
            "lock_hash": schemalib.lock_hash(t, m["domain"]),
            "cpp": "sl::fabric::gen::%s::%s" % (ns_of(m["module"]), model.cpp_local(t["name"])),
            "fields": [_meta_field(model, t, f) for f in sorted(t.get("fields", []), key=lambda x: x["no"])],
        }
        if t.get("reserved"):
            entry["reserved"] = sorted(set(t["reserved"]))
        if t.get("enumerators"):
            entry["enumerators"] = [_meta_member(e) for e in sorted(t["enumerators"], key=lambda x: x["no"])]
        if t.get("cases"):
            entry["cases"] = [dict(_meta_member(c), type=c["type"]) for c in sorted(t["cases"], key=lambda x: x["no"])]
        for key in ("element", "key"):
            if key in t:
                entry[key] = t[key]
        types.append(entry)
    return {"format": META_FORMAT, "module": m["module"], "domain": m["domain"], "owner": m["owner"],
            "doc": m.get("doc", ""), "source": model.rel[m["module"]], "types": types}


def _meta_field(model, t, f):
    """A field: its own doc and presence, and the domain, owner and state class that govern its value (ARCH1-R39:
    the innermost typed value whose domain differs from its parent's)."""
    holder = f["type"] if f["type"] in model.types else t["name"]
    domain = model.domain(holder)
    out = {"no": f["no"], "name": f["name"], "cpp": member_name(f["name"], model.cpp_local(t["name"])),
           "type": f["type"], "presence": f["presence"], "doc": f.get("doc", ""), "domain": domain,
           "owner": model.owner(domain), "state_class": model.types[holder]["state_class"],
           "since_minor": f.get("since_minor", 0)}
    if "deprecated_in" in f:
        out["deprecated_in"] = f["deprecated_in"]
    if "default" in f:
        out["default"] = f["default"]
    out["ref_strength"] = f["ref"]["strength"] if "ref" in f else None
    out["ref_targets"] = sorted(f["ref"]["targets"]) if "ref" in f else []
    return out


def _meta_member(e):
    out = {"no": e["no"], "name": e["name"], "doc": e.get("doc", ""), "since_minor": e.get("since_minor", 0)}
    if "deprecated_in" in e:
        out["deprecated_in"] = e["deprecated_in"]
    return out


# ---- text schema -----------------------------------------------------------------------------------------------------

def _digits_range(a, b):
    """Regex alternatives for the decimal strings of equal length a..b (as strings)."""
    if a == b:
        return [a]
    if len(a) == 1:
        return ["[%s-%s]" % (a, b)]
    if a[1:] == "0" * (len(a) - 1) and b[1:] == "9" * (len(b) - 1):
        head = a[0] if a[0] == b[0] else "[%s-%s]" % (a[0], b[0])
        return ["%s[0-9]{%d}" % (head, len(a) - 1)]
    if a[0] == b[0]:
        return [a[0] + p for p in _digits_range(a[1:], b[1:])]
    out = [a[0] + p for p in _digits_range(a[1:], "9" * (len(a) - 1))]
    lo, hi = int(a[0]) + 1, int(b[0]) - 1
    if lo <= hi:
        head = str(lo) if lo == hi else "[%d-%d]" % (lo, hi)
        out.append("%s[0-9]{%d}" % (head, len(a) - 1))
    out += [b[0] + p for p in _digits_range("0" * (len(b) - 1), b[1:])]
    return out


def range_regex(lo, hi):
    """Regex alternation (no anchors) of the decimal spellings of the integers lo..hi, 0 <= lo <= hi."""
    parts = []
    while lo <= hi:
        top = min(hi, 10 ** len(str(lo)) - 1)
        parts += _digits_range(str(lo), str(top))
        lo = top + 1
    return "|".join(parts)


def _int_schema(ref):
    low, high = schemalib.INT_RANGES[ref]
    number = {"type": "integer", "minimum": max(low, -MAX_SAFE), "maximum": min(high, MAX_SAFE)}
    if high <= MAX_SAFE and low >= -MAX_SAFE:
        return number
    alts = []
    if high > MAX_SAFE:
        alts.append(range_regex(MAX_SAFE + 1, high))
    if low < -MAX_SAFE:
        alts.append("-(?:%s)" % range_regex(MAX_SAFE + 1, -low))
    pattern = "^(?:%s)$" % "|".join("(?:%s)" % a for a in alts)
    return {"anyOf": [number, {"type": "string", "pattern": pattern}]}


def id_pattern(classes=None):
    """Id text of the given class names (every class when None): prefix, the class byte as its two Crockford
    characters, a scheme byte of 1 to 4, then the payload."""
    names = dict((c, p) for c, p in ID_PREFIXES)
    numbers = [c for c, _p in ID_PREFIXES] if classes is None else sorted(
        c for c, _p in ID_PREFIXES if CLASS_NAMES[c] in classes)
    return "^(?:%s)0[4-9A-HJK][0-9A-HJKMNP-TV-Z]{22}$" % "|".join("%s_0%s" % (names[c], CROCKFORD[c])
                                                                  for c in numbers)


CLASS_NAMES = {1: "project", 2: "schema", 3: "type", 4: "asset", 5: "entity", 6: "instance", 7: "derived", 8: "tx",
               9: "event", 10: "actor", 11: "package", 12: "element", 13: "namespace", 14: "operation", 15: "import"}
ID_PATTERN = id_pattern()
DIGEST_PATTERN = "^(?:sha256|blake3):[0-9a-f]{64}$"
HEX_PATTERN = "^(?:[0-9a-f]{2})*$"
ORDER_KEY_PATTERN = "^(?:[0-9a-f]{2})*(?:0[1-9a-f]|[1-9a-f][0-9a-f])$"


def _scalar_schema(ref):
    if ref in schemalib.INT_RANGES:
        return _int_schema(ref)
    simple = {"bool": {"type": "boolean"}, "f32": {"type": "number"}, "f64": {"type": "number"},
              "string": {"type": "string"}, "bytes": {"type": "string", "pattern": HEX_PATTERN},
              "id": {"type": "string", "pattern": ID_PATTERN}, "ref": {"type": "string", "pattern": ID_PATTERN},
              "digest": {"type": "string", "pattern": DIGEST_PATTERN},
              "blob_ref": {"type": "string", "pattern": DIGEST_PATTERN}}
    if ref in simple:
        return dict(simple[ref])
    # external_ref: the built-in struct {1 system string, 2 locator string, 3 content digest optional}
    return {"type": "object", "properties": {"system": {"type": "string"}, "locator": {"type": "string"},
                                             "content": {"type": "string", "pattern": DIGEST_PATTERN}},
            "required": ["system", "locator"], "additionalProperties": False}


def _ref_schema(model, ref, field=None):
    if ref in model.types:
        return {"$ref": "#/$defs/%s" % ref}
    if ref == "ref" and field is not None:
        classes = {model.types[t]["record_class"] for t in field["ref"]["targets"]}
        return {"type": "string", "pattern": id_pattern(classes)}
    return _scalar_schema(ref)


def _type_schema(model, t):
    kind = t["kind"]
    if kind == "struct":
        props, required = {}, []
        for f in sorted(t.get("fields", []), key=lambda x: x["no"]):
            s = _ref_schema(model, f["type"], f)
            if f.get("doc"):
                s = dict(s, description=f["doc"])
            props[f["name"]] = s
            if f["presence"] == "required":
                required.append(f["name"])
        out = {"type": "object", "properties": props, "required": required, "additionalProperties": False}
    elif kind == "enum":
        out = {"enum": [e["name"] for e in sorted(t["enumerators"], key=lambda x: x["no"])]}
    elif kind == "variant":
        alts = [{"type": "object", "properties": {c["name"]: _ref_schema(model, c["type"])}, "required": [c["name"]],
                 "additionalProperties": False} for c in sorted(t["cases"], key=lambda x: x["no"])]
        out = {"oneOf": alts}
    elif kind == "list":
        out = {"type": "array", "items": _ref_schema(model, t["element"])}
    elif kind == "oset":
        out = {"type": "array", "items": {"type": "object", "properties": {
            "id": {"type": "string", "pattern": id_pattern({"element"})},
            "order": {"type": "string", "pattern": ORDER_KEY_PATTERN},
            "value": _ref_schema(model, t["element"])}, "required": ["id", "order", "value"],
            "additionalProperties": False}}
    elif kind == "map":
        out = {"type": "array", "items": {"type": "array", "prefixItems": [
            _ref_schema(model, t["key"]), _ref_schema(model, t["element"])], "minItems": 2, "maxItems": 2}}
    else:
        out = _ref_schema(model, t["element"])
    if t.get("doc"):
        out = dict(out, description=t["doc"])
    return out


def render_text_schema(model, m):
    names, todo = [], [t["name"] for t in m["types"]]
    while todo:
        name = todo.pop(0)
        if name not in names:
            names.append(name)
            todo += model.deps(name)
    defs = {name: _type_schema(model, model.types[name]) for name in sorted(names)}
    return {"$schema": "https://json-schema.org/draft/2020-12/schema",
            "$id": "https://slicedlabs.example/gen/%s.text.schema.json" % m["module"],
            "title": "Canonical text of module %s (ARCH1-R29, generated by tools/fabric/slgen.py)" % m["module"],
            "$defs": defs}


# ---- the generated set -----------------------------------------------------------------------------------------------

def _cpp(lines, rel, findings):
    for no, line in enumerate(lines, 1):
        if len(line) > COLUMNS:
            findings.append("%s:%d: R-GEN-LONG: line of %d columns (limit %d): %s" % (rel, no, len(line), COLUMNS,
                                                                                    line.strip()[:60]))
    return "\n".join(lines) + "\n"


def _json(doc):
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def check_supported(model, require_docs):
    findings = list(model.findings)
    for m in model.modules:
        rel = model.rel[m["module"]]
        for t in m["types"]:
            subject = t["name"]
            for key in ("enumerators", "cases"):
                for e in t.get(key, []):
                    if e.get("since_minor", 0) or "deprecated_in" in e:
                        findings.append("%s: R-GEN-UNSUPPORTED: %s %s %s: since_minor and deprecated_in of "
                                        "enumerators and cases are not in the C++ descriptors" % (rel, subject,
                                                                                                  key[:-1], e["name"]))
            if t["kind"] == "list" and model.identity_bearing(t["element"]):
                findings.append("%s: R-GEN-LIST-ID: %s is a list of the identity-bearing %s; use an oset (ARCH1-R15)"
                                % (rel, subject, t["element"]))
            if require_docs:
                missing = [] if t.get("doc") else ["the type"]
                missing += ["field %s" % f["name"] for f in t.get("fields", []) if not f.get("doc")]
                missing += ["enumerator %s" % e["name"] for e in t.get("enumerators", []) if not e.get("doc")]
                missing += ["case %s" % c["name"] for c in t.get("cases", []) if not c.get("doc")]
                if missing:
                    findings.append("%s: R-GEN-DOC: %s has no doc on %s" % (rel, subject, ", ".join(missing)))
    return findings


def generate_set(root, modules_dir=None, rel_dir=MODULES_REL, require_docs=True):
    """Return {relative path: text} of every file the set produces; GenError when the set is refused."""
    modules_dir = modules_dir or os.path.join(root, *MODULES_REL.split("/"))
    modules, arch = load_set(root, modules_dir, rel_dir)
    model = Model(modules, arch)
    findings = check_supported(model, require_docs)
    if findings:
        raise GenError(findings)
    model.module_order()
    files = {}
    gen = GEN_REL
    files["%s/%s/support.gen.hpp" % (gen, HEADER_DIR)] = _cpp(SUPPORT_HPP.split("\n")[:-1], "support.gen.hpp",
                                                                findings)
    files["%s/src/support.gen.cpp" % gen] = _cpp(SUPPORT_CPP.split("\n")[:-1], "support.gen.cpp", findings)
    for m in model.modules:
        mod = m["module"]
        rel = "%s/%s/%s.gen.hpp" % (gen, HEADER_DIR, mod)
        files[rel] = _cpp(render_header(model, m), rel, findings)
        rel = "%s/src/%s.gen.cpp" % (gen, mod)
        files[rel] = _cpp(render_source(model, m), rel, findings)
        files["%s/meta/%s.meta.json" % (gen, mod)] = _json(render_meta(model, m))
        files["%s/text/%s.text.schema.json" % (gen, mod)] = _json(render_text_schema(model, m))
    rel = "%s/%s/system.gen.hpp" % (gen, HEADER_DIR)
    files[rel] = _cpp(render_system_header(model), rel, findings)
    rel = "%s/src/system.gen.cpp" % gen
    files[rel] = _cpp(render_system_source(model), rel, findings)
    hashes = {t["name"]: schemalib.lock_hash(t, m["domain"]) for m in model.modules for t in m["types"]}
    files["%s/%s" % (FIXTURES_REL, LOCK_HASHES)] = _json({"format": "sl-gen-lock-hashes/1",
                                                          "hashes": dict(sorted(hashes.items()))})
    if findings:
        raise GenError(findings)
    return files, model


# ---- fixtures ----------------------------------------------------------------------------------------------------

def text_schemas(model):
    """The module set as slb_text corpus descriptors (tagged is its base, ref an id, blob_ref a digest)."""
    def ref(name):
        if name in model.types:
            return name
        if name in schemalib.INT_RANGES:
            low, high = schemalib.INT_RANGES[name]
            return {"kind": "int" if low < 0 else "uint", "bits": int(name[1:])}
        if name == "external_ref":
            return {"kind": "struct", "fields": [{"no": 1, "name": "system", "type": {"kind": "string"}},
                                                 {"no": 2, "name": "locator", "type": {"kind": "string"}},
                                                 {"no": 3, "name": "content", "type": {"kind": "digest"},
                                                  "optional": True}]}
        return {"kind": {"ref": "id", "blob_ref": "digest"}.get(name, name)}

    out = {}
    for name, t in model.types.items():
        kind = t["kind"]
        if kind == "struct":
            out[name] = {"kind": "struct", "fields": [
                {"no": f["no"], "name": f["name"], "type": ref(f["type"]), "optional": f["presence"] == "optional"}
                for f in t.get("fields", [])]}
        elif kind == "enum":
            out[name] = {"kind": "enum", "enumerators": [{"no": e["no"], "name": e["name"]}
                                                         for e in t["enumerators"]]}
        elif kind == "variant":
            out[name] = {"kind": "variant", "cases": [{"no": c["no"], "name": c["name"], "type": ref(c["type"])}
                                                      for c in t["cases"]]}
        elif kind in ("list", "oset"):
            out[name] = {"kind": kind, "element": ref(t["element"])}
        elif kind == "map":
            key = t["key"]
            key_kind = key if key in ("string", "id") else ("int" if key.startswith("i") else "uint")
            out[name] = {"kind": "map", "key": key_kind, "value": ref(t["element"])}
    for name, t in model.types.items():
        if t["kind"] == "tagged":
            base = name
            while base in model.types and model.types[base]["kind"] == "tagged":
                base = model.types[base]["element"]
            out[name] = out[base] if base in out else ref(base)
    return out


def compact(tree):
    return json.dumps(tree, ensure_ascii=False, separators=(",", ":"))


def check_fixtures(root, model):
    """R-GEN-FIXTURE findings: every type has >= 2 canonical text values; no fixture names an unknown type."""
    findings = []
    base = os.path.join(root, *FIXTURES_REL.split("/"))
    schemas = text_schemas(model)
    expected = {}
    for m in model.modules:
        for t in m["types"]:
            expected["%s/%s.json" % (m["module"], model.local(t["name"]))] = t["name"]
    for sub, name in sorted(expected.items()):
        rel = "%s/%s" % (FIXTURES_REL, sub)
        try:
            with open(os.path.join(base, *sub.split("/")), encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, ValueError) as exc:
            findings.append("%s: R-GEN-FIXTURE: %s needs fixtures: %s" % (rel, name, exc))
            continue
        values = doc.get("values") if isinstance(doc, dict) else None
        if doc.get("format") != FIXTURE_FORMAT or doc.get("type") != name or not isinstance(values, list):
            findings.append('%s: R-GEN-FIXTURE: expected {"format": "%s", "type": "%s", "values": [...]}'
                            % (rel, FIXTURE_FORMAT, name))
            continue
        if len(values) < 2:
            findings.append("%s: R-GEN-FIXTURE: %s has %d value(s), at least 2 are required" % (rel, name, len(values)))
        texts = []
        for i, tree in enumerate(values):
            text = compact(tree)
            try:
                slb_text.from_text(text, name, schemas)
            except (SlbError, ValueError, KeyError, TypeError) as exc:
                findings.append("%s: R-GEN-FIXTURE: value %d is not canonical text of %s: %s" % (rel, i, name, exc))
            if text in texts:
                findings.append("%s: R-GEN-FIXTURE: value %d repeats an earlier value" % (rel, i))
            texts.append(text)
    if os.path.isdir(base):
        for directory in sorted(os.listdir(base)):
            full = os.path.join(base, directory)
            if not os.path.isdir(full):
                continue
            for fname in sorted(os.listdir(full)):
                sub = "%s/%s" % (directory, fname)
                if sub not in expected:
                    findings.append("%s/%s: R-GEN-FIXTURE: names no generated type" % (FIXTURES_REL, sub))
    return findings


# ---- check and generate ------------------------------------------------------------------------------------------

def generated_paths_on_disk(root):
    out = []
    for sub in GEN_DIRS:
        directory = os.path.join(root, *GEN_REL.split("/"), *sub.split("/"))
        if os.path.isdir(directory):
            out += ["%s/%s/%s" % (GEN_REL, sub, n) for n in sorted(os.listdir(directory))
                    if os.path.isfile(os.path.join(directory, n)) and n != "README.md"]
    return out


def _first_difference(old, new):
    a, b = old.split("\n"), new.split("\n")
    for i, (x, y) in enumerate(zip(a, b), 1):
        if x != y:
            return i
    return min(len(a), len(b)) + 1


def check(root):
    """Return (findings, counts); counts is (modules, types, files)."""
    try:
        files, model = generate_set(root)
    except GenError as exc:
        return exc.findings, (0, 0, 0)
    findings = []
    for rel in sorted(files):
        path = os.path.join(root, *rel.split("/"))
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                current = fh.read()
        except OSError:
            findings.append("%s: R-GEN-MISSING: not generated (run `python3 -I tools/fabric/slgen.py generate`)" % rel)
            continue
        if current != files[rel]:
            findings.append("%s: R-GEN-STALE: differs from slgen output at line %d (never edit generated files; run "
                            "`python3 -I tools/fabric/slgen.py generate`)" % (rel, _first_difference(current,
                                                                                                   files[rel])))
    for rel in generated_paths_on_disk(root):
        if rel not in files:
            findings.append("%s: R-GEN-EXTRA: in a generated directory but not written by slgen" % rel)
    findings += check_fixtures(root, model)
    return findings, (len(model.modules), len(model.types), len(files))


def generate(root):
    files, model = generate_set(root)
    written = 0
    for rel, text in sorted(files.items()):
        path = os.path.join(root, *rel.split("/"))
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                if fh.read() == text:
                    continue
        except OSError:
            pass
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        written += 1
    removed = 0
    for rel in generated_paths_on_disk(root):
        if rel not in files:
            os.remove(os.path.join(root, *rel.split("/")))
            removed += 1
    return written, removed, model


def main(argv):
    root, args, command = ROOT, argv[1:], None
    if args and args[0] in ("generate", "check"):
        command, args = args[0], args[1:]
    if args[:1] == ["--root"] and len(args) == 2:
        root = os.path.abspath(args[1])
    elif args or command is None:
        print("usage: slgen.py generate|check [--root DIR]", file=sys.stderr)
        return 2
    if command == "generate":
        try:
            written, removed, model = generate(root)
        except GenError as exc:
            print("\n".join(exc.findings))
            print("slgen: refused (%d finding(s))" % len(exc.findings))
            return 1
        print("slgen: %d modules, %d types; wrote %d file(s), removed %d" % (len(model.modules), len(model.types),
                                                                           written, removed))
        return 0
    findings, (modules, types, files) = check(root)
    for f in findings:
        print(f)
    print("fabric-gen-check: %s" % ("FAIL (%d finding(s))" % len(findings) if findings else "ok"))
    print("modules %d types %d files %d" % (modules, types, files))
    return 1 if findings else 0


# ---- static support files (clang-format clean) ---------------------------------------------------------------------

SUPPORT_HPP = r'''#pragma once

// Generated by tools/fabric/slgen.py (WO-045) as static text.
// Do not edit: `python3 -I tools/fabric/slgen.py generate` rewrites it and `just fabric-gen-check`
// fails when it drifts (SL-ARCH-001 ARCH1-R67, ARCH1-R68, ADR-0013).
//
// Support for the schema-generated modules: the oset element and external_ref types, the type
// table entry, descriptor builders, and the conversions between typed C++ values and canonical
// values (value.hpp). Every conversion returns sl::result. Decoding refuses with `type_mismatch` a
// value of the wrong kind, an integer outside its width, an unknown enumerator or case, an id that
// is not a valid record id, a missing required field and a field number the type does not declare
// (newer-minor fields included: typed values are strict, the store preserves raw values).
// Encoding refuses a null id and what the value constructors refuse (a non-finite float, ill-formed
// UTF-8).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::gen {

/// One element of a generated oset: element id, fractional order key and value (ARCH1-R15).
template <class T> struct oset_item {
  record_id element;
  std::vector<std::uint8_t> order_key;
  T v{};

  friend bool operator==(const oset_item &, const oset_item &) = default;
};

/// The built-in external_ref: struct {1 system string, 2 locator string, 3 content digest
/// optional}.
struct external_ref {
  std::string system;
  std::string locator;
  std::optional<digest> content;

  friend bool operator==(const external_ref &, const external_ref &) = default;
};

/// One generated type: its qualified name, the state class its schema declares, and a round trip
/// through its typed C++ value (from_value, then to_value).
struct type_entry {
  std::string_view name;
  /// The sl-type-def/1 state class. A registered descriptor keeps a state class only on record
  /// types (type.hpp: a state class makes a type a record type), so lock_hash restores this one.
  state_class declared_state = state_class::none;
  sl::result<value> (*roundtrip)(const value &) = nullptr;
};

/// The schema object id of a registered generated type as tools/fabric/schemalib.py computes its
/// lock hash: the descriptor with its declared state class (FAB-INV-GEN-03). `schema_unknown`
/// when the type is not registered.
[[nodiscard]] sl::result<digest> lock_hash(const type_entry &entry, const schema_registry &schemas);

namespace detail {

/// `type_mismatch`: "<where>: <message>".
[[nodiscard]] std::unexpected<error> mismatch(std::string_view where, std::string_view message);
/// `type_mismatch`: "expected <expected>, got <kind of v>".
[[nodiscard]] std::unexpected<error> mismatch(std::string_view expected, const value &v);

// ---- descriptors ---------------------------------------------------------------------------

[[nodiscard]] type_desc make_type(std::string_view name);
[[nodiscard]] field_desc make_field(std::uint32_t no, std::string_view name);
[[nodiscard]] enumerator_desc make_enumerator(std::uint32_t no, std::string_view name);
[[nodiscard]] case_desc make_case(std::uint32_t no, std::string_view name);
[[nodiscard]] ref_spec make_ref(ref_strength strength);
/// Constants of field defaults; the generator writes only finite floats and valid strings.
[[nodiscard]] value f32_constant(float v);
[[nodiscard]] value f64_constant(double v);
[[nodiscard]] value string_constant(std::string_view s);
/// Adds every descriptor in order; the first refusal is returned.
[[nodiscard]] sl::result<void> add_all(schema_registry &reg, std::vector<type_desc> all);
[[nodiscard]] type_entry entry(std::string_view name,
                               sl::result<value> (*roundtrip)(const value &));
void append(std::vector<type_entry> &out, const std::vector<type_entry> &more);

// ---- scalars ---------------------------------------------------------------------------------

[[nodiscard]] sl::result<value> enc_bool(const bool &v);
[[nodiscard]] sl::result<value> enc_f32(const float &v);
[[nodiscard]] sl::result<value> enc_f64(const double &v);
[[nodiscard]] sl::result<value> enc_string(const std::string &v);
[[nodiscard]] sl::result<value> enc_bytes(const std::vector<std::uint8_t> &v);
[[nodiscard]] sl::result<value> enc_id(const record_id &v);
[[nodiscard]] sl::result<value> enc_digest(const digest &v);
[[nodiscard]] sl::result<value> enc_external_ref(const external_ref &v);

[[nodiscard]] sl::result<bool> dec_bool(const value &v);
[[nodiscard]] sl::result<float> dec_f32(const value &v);
[[nodiscard]] sl::result<double> dec_f64(const value &v);
[[nodiscard]] sl::result<std::string> dec_string(const value &v);
[[nodiscard]] sl::result<std::vector<std::uint8_t>> dec_bytes(const value &v);
[[nodiscard]] sl::result<record_id> dec_id(const value &v);
[[nodiscard]] sl::result<digest> dec_digest(const value &v);
[[nodiscard]] sl::result<external_ref> dec_external_ref(const value &v);

template <class Int> [[nodiscard]] sl::result<value> enc_int(const Int &v) {
  if constexpr (std::is_signed_v<Int>) {
    return value::integer(static_cast<std::int64_t>(v));
  } else {
    return value::uint(static_cast<std::uint64_t>(v));
  }
}

template <class Int> [[nodiscard]] sl::result<Int> dec_int(const value &v) {
  if constexpr (std::is_signed_v<Int>) {
    const std::optional<std::int64_t> n = v.as_i64();
    if (!n || *n < std::numeric_limits<Int>::min() || *n > std::numeric_limits<Int>::max()) {
      return mismatch("a signed integer in range", v);
    }
    return static_cast<Int>(*n);
  } else {
    if (v.kind() != value_kind::uint || v.as_uint() > std::numeric_limits<Int>::max()) {
      return mismatch("an unsigned integer in range", v);
    }
    return static_cast<Int>(v.as_uint());
  }
}

// ---- enums and variants ----------------------------------------------------------------------

template <class E> [[nodiscard]] sl::result<value> enc_enum(const E &v) {
  return value::uint(static_cast<std::uint64_t>(std::to_underlying(v)));
}

/// The enumerator number of `v`; `type_mismatch` unless it is a uint of at most 32 bits.
[[nodiscard]] sl::result<std::uint32_t> dec_enumerator(const value &v, std::string_view type);
[[nodiscard]] std::unexpected<error> unknown_enumerator(std::string_view type, std::uint32_t n);
/// The case number of `v`; `type_mismatch` unless it is a variant.
[[nodiscard]] sl::result<std::uint32_t> dec_case(const value &v, std::string_view type);
[[nodiscard]] std::unexpected<error> unknown_case(std::string_view type, std::uint32_t n);
[[nodiscard]] std::unexpected<error> valueless(std::string_view type);

template <class T, class F>
[[nodiscard]] sl::result<value> enc_case(std::uint32_t no, const T &v, const F &enc) {
  auto payload = enc(v);
  if (!payload) {
    return std::unexpected(std::move(payload.error()));
  }
  return value::variant(no, std::move(*payload));
}

template <class V, std::size_t I, class F>
[[nodiscard]] sl::result<V> dec_alt(const value &payload, const F &dec) {
  auto x = dec(payload);
  if (!x) {
    return std::unexpected(std::move(x.error()));
  }
  V out;
  out.alt.template emplace<I>(std::move(*x));
  return out;
}

// ---- containers ------------------------------------------------------------------------------

/// The C++ type a decoder produces.
template <class F>
using decoded_t = typename std::invoke_result_t<const F &, const value &>::value_type;

template <class T, class F>
[[nodiscard]] sl::result<value> enc_list(const std::vector<T> &in, const F &enc) {
  std::vector<value> items;
  items.reserve(in.size());
  for (const T &x : in) {
    auto v = enc(x);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    items.push_back(std::move(*v));
  }
  return value::list(std::move(items));
}

template <class F>
[[nodiscard]] sl::result<std::vector<decoded_t<F>>> dec_list(const value &in, const F &dec) {
  if (in.kind() != value_kind::list) {
    return mismatch("list", in);
  }
  std::vector<decoded_t<F>> out;
  out.reserve(in.items().size());
  for (const value &x : in.items()) {
    auto v = dec(x);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    out.push_back(std::move(*v));
  }
  return out;
}

template <class T, class F>
[[nodiscard]] sl::result<value> enc_oset(const std::vector<oset_item<T>> &in, const F &enc) {
  std::vector<oset_entry> entries;
  entries.reserve(in.size());
  for (const oset_item<T> &x : in) {
    if (x.element.is_null()) {
      return mismatch("oset", "an element id is null");
    }
    auto v = enc(x.v);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    entries.push_back(
        oset_entry{.element = x.element.bytes(), .order_key = x.order_key, .v = std::move(*v)});
  }
  return value::oset(std::move(entries));
}

template <class F>
[[nodiscard]] sl::result<std::vector<oset_item<decoded_t<F>>>> dec_oset(const value &in,
                                                                        const F &dec) {
  if (in.kind() != value_kind::oset) {
    return mismatch("oset", in);
  }
  std::vector<oset_item<decoded_t<F>>> out;
  out.reserve(in.elements().size());
  for (const oset_entry &e : in.elements()) {
    const auto element = record_id::from_bytes(e.element);
    if (!element) {
      return mismatch("oset", "an element id is not a valid record id");
    }
    auto v = dec(e.v);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    out.push_back(
        oset_item<decoded_t<F>>{.element = *element, .order_key = e.order_key, .v = std::move(*v)});
  }
  return out;
}

template <class K, class V, class FK, class FV>
[[nodiscard]] sl::result<value> enc_map(const std::map<K, V> &in, const FK &enc_key,
                                        const FV &enc_value) {
  std::vector<map_entry> entries;
  entries.reserve(in.size());
  for (const auto &[k, x] : in) {
    auto key = enc_key(k);
    if (!key) {
      return std::unexpected(std::move(key.error()));
    }
    auto v = enc_value(x);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    entries.push_back(map_entry{.key = std::move(*key), .v = std::move(*v)});
  }
  return value::map(std::move(entries));
}

template <class FK, class FV>
[[nodiscard]] sl::result<std::map<decoded_t<FK>, decoded_t<FV>>>
dec_map(const value &in, const FK &dec_key, const FV &dec_value) {
  if (in.kind() != value_kind::map) {
    return mismatch("map", in);
  }
  std::map<decoded_t<FK>, decoded_t<FV>> out;
  for (const map_entry &e : in.entries()) {
    auto key = dec_key(e.key);
    if (!key) {
      return std::unexpected(std::move(key.error()));
    }
    auto v = dec_value(e.v);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    out.insert_or_assign(std::move(*key), std::move(*v));
  }
  return out;
}

// ---- structs ---------------------------------------------------------------------------------

/// Collects the fields of a struct value in any order; the first refusal wins.
class struct_writer {
public:
  template <class T, class F> void put(std::uint32_t no, const T &v, const F &enc) {
    if (error_) {
      return;
    }
    auto r = enc(v);
    if (!r) {
      fail(no, std::move(r.error()));
      return;
    }
    fields_.push_back(field{.no = no, .v = std::move(*r)});
  }

  template <class T, class F>
  void put_optional(std::uint32_t no, const std::optional<T> &v, const F &enc) {
    if (v) {
      put(no, *v, enc);
    }
  }

  /// The struct value, or the first refusal.
  [[nodiscard]] sl::result<value> finish();

private:
  void fail(std::uint32_t no, error e);

  std::vector<field> fields_;
  std::optional<error> error_;
};

/// Reads the fields of a struct value of type `type`: every get names a declared field, and
/// finish() refuses a field the gets did not name (R-TY-FIELD, R-TY-RESERVED).
class struct_reader {
public:
  struct_reader(const value &in, std::string_view type);

  template <class T, class F> void get(std::uint32_t no, T &dst, const F &dec) {
    known_.push_back(no);
    if (error_) {
      return;
    }
    const value *f = in_->find_field(no);
    if (f == nullptr) {
      fail(no, "required field is absent");
      return;
    }
    read(no, *f, dst, dec);
  }

  template <class T, class F>
  void get_optional(std::uint32_t no, std::optional<T> &dst, const F &dec) {
    known_.push_back(no);
    if (error_) {
      return;
    }
    if (const value *f = in_->find_field(no); f != nullptr) {
      T x{};
      read(no, *f, x, dec);
      dst = std::move(x);
    }
  }

  /// `out` when every field was read and none is undeclared; the first refusal otherwise.
  template <class T> [[nodiscard]] sl::result<T> finish(T out) {
    if (auto r = done(); !r) {
      return std::unexpected(std::move(r.error()));
    }
    return out;
  }

private:
  template <class T, class F> void read(std::uint32_t no, const value &f, T &dst, const F &dec) {
    auto r = dec(f);
    if (!r) {
      fail(no, r.error().message);
      return;
    }
    dst = std::move(*r);
  }

  void fail(std::uint32_t no, std::string_view message);
  [[nodiscard]] sl::result<void> done() const;

  const value *in_;
  std::string_view type_;
  std::vector<std::uint32_t> known_;
  std::optional<error> error_;
};

} // namespace detail

} // namespace sl::fabric::gen
'''

SUPPORT_CPP = r'''// Generated by tools/fabric/slgen.py (WO-045) as static text.
// Do not edit: `python3 -I tools/fabric/slgen.py generate` rewrites it and `just fabric-gen-check`
// fails when it drifts (SL-ARCH-001 ARCH1-R67, ARCH1-R68, ADR-0013).

#include "sl/fabric/gen/support.gen.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::gen {

sl::result<digest> lock_hash(const type_entry &entry, const schema_registry &schemas) {
  const type_desc *t = schemas.find(entry.name);
  if (t == nullptr) {
    return make_error(errc::schema_unknown, std::format("type {} is not registered", entry.name));
  }
  type_desc declared = *t;
  declared.state_class = entry.declared_state;
  return schema_object_id(declared, schemas);
}

namespace detail {

std::unexpected<error> mismatch(std::string_view where, std::string_view message) {
  return make_error(errc::type_mismatch, std::format("{}: {}", where, message));
}

std::unexpected<error> mismatch(std::string_view expected, const value &v) {
  return make_error(errc::type_mismatch,
                    std::format("expected {}, got {}", expected, to_string(v.kind())));
}

type_desc make_type(std::string_view name) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  return t;
}

field_desc make_field(std::uint32_t no, std::string_view name) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  return f;
}

enumerator_desc make_enumerator(std::uint32_t no, std::string_view name) {
  return enumerator_desc{.no = no, .name = std::string(name), .doc = {}};
}

case_desc make_case(std::uint32_t no, std::string_view name) {
  return case_desc{.no = no, .name = std::string(name), .type = {}, .doc = {}};
}

ref_spec make_ref(ref_strength strength) { return ref_spec{.strength = strength, .targets = {}}; }

value f32_constant(float v) { return value::f32(v).value_or(value{}); }

value f64_constant(double v) { return value::f64(v).value_or(value{}); }

value string_constant(std::string_view s) { return value::string(s).value_or(value{}); }

sl::result<void> add_all(schema_registry &reg, std::vector<type_desc> all) {
  for (type_desc &t : all) {
    if (auto r = reg.add(std::move(t)); !r) {
      return r;
    }
  }
  return {};
}

type_entry entry(std::string_view name, sl::result<value> (*roundtrip)(const value &)) {
  return type_entry{.name = name, .declared_state = state_class::none, .roundtrip = roundtrip};
}

void append(std::vector<type_entry> &out, const std::vector<type_entry> &more) {
  out.insert(out.end(), more.begin(), more.end());
}

sl::result<value> enc_bool(const bool &v) { return value::boolean(v); }

sl::result<value> enc_f32(const float &v) { return value::f32(v); }

sl::result<value> enc_f64(const double &v) { return value::f64(v); }

sl::result<value> enc_string(const std::string &v) { return value::string(v); }

sl::result<value> enc_bytes(const std::vector<std::uint8_t> &v) { return value::bytes(v); }

sl::result<value> enc_id(const record_id &v) {
  if (v.is_null()) {
    return mismatch("id", "a null id has no canonical value");
  }
  return value::id(v);
}

sl::result<value> enc_digest(const digest &v) { return value::digest(v); }

sl::result<value> enc_external_ref(const external_ref &v) {
  struct_writer w;
  w.put(1, v.system, enc_string);
  w.put(2, v.locator, enc_string);
  w.put_optional(3, v.content, enc_digest);
  return w.finish();
}

sl::result<bool> dec_bool(const value &v) {
  if (v.kind() != value_kind::bool_) {
    return mismatch("bool", v);
  }
  return v.as_bool();
}

sl::result<float> dec_f32(const value &v) {
  if (v.kind() != value_kind::f32) {
    return mismatch("f32", v);
  }
  return v.as_f32();
}

sl::result<double> dec_f64(const value &v) {
  if (v.kind() != value_kind::f64) {
    return mismatch("f64", v);
  }
  return v.as_f64();
}

sl::result<std::string> dec_string(const value &v) {
  if (v.kind() != value_kind::string) {
    return mismatch("string", v);
  }
  return std::string(v.as_string());
}

sl::result<std::vector<std::uint8_t>> dec_bytes(const value &v) {
  if (v.kind() != value_kind::bytes) {
    return mismatch("bytes", v);
  }
  const byte_span b = v.as_bytes();
  return std::vector<std::uint8_t>(b.begin(), b.end());
}

sl::result<record_id> dec_id(const value &v) {
  if (v.kind() != value_kind::id) {
    return mismatch("id", v);
  }
  auto id = v.as_record_id();
  if (!id) {
    return mismatch("id", "not a valid record id");
  }
  return *id;
}

sl::result<digest> dec_digest(const value &v) {
  if (v.kind() != value_kind::digest) {
    return mismatch("digest", v);
  }
  return v.as_digest();
}

sl::result<external_ref> dec_external_ref(const value &v) {
  struct_reader r(v, "external_ref");
  external_ref out;
  r.get(1, out.system, dec_string);
  r.get(2, out.locator, dec_string);
  r.get_optional(3, out.content, dec_digest);
  return r.finish(std::move(out));
}

sl::result<std::uint32_t> dec_enumerator(const value &v, std::string_view type) {
  if (v.kind() != value_kind::uint || v.as_uint() > std::numeric_limits<std::uint32_t>::max()) {
    return mismatch(type,
                    std::format("expected an enumerator number, got {}", to_string(v.kind())));
  }
  return static_cast<std::uint32_t>(v.as_uint());
}

std::unexpected<error> unknown_enumerator(std::string_view type, std::uint32_t n) {
  return mismatch(type, std::format("{} is not an enumerator", n));
}

sl::result<std::uint32_t> dec_case(const value &v, std::string_view type) {
  if (v.kind() != value_kind::variant) {
    return mismatch(type, std::format("expected a variant, got {}", to_string(v.kind())));
  }
  return v.case_no();
}

std::unexpected<error> unknown_case(std::string_view type, std::uint32_t n) {
  return mismatch(type, std::format("{} is not a case", n));
}

std::unexpected<error> valueless(std::string_view type) {
  return mismatch(type, "the variant holds no alternative");
}

sl::result<value> struct_writer::finish() {
  if (error_) {
    return std::unexpected(std::move(*error_));
  }
  return value::struct_(std::move(fields_));
}

void struct_writer::fail(std::uint32_t no, error e) {
  e.message = std::format("field {}: {}", no, e.message);
  error_ = std::move(e);
}

struct_reader::struct_reader(const value &in, std::string_view type) : in_(&in), type_(type) {
  if (in.kind() != value_kind::struct_) {
    error_ = mismatch(type, std::format("expected a struct, got {}", to_string(in.kind()))).error();
  }
}

void struct_reader::fail(std::uint32_t no, std::string_view message) {
  error_ = mismatch(type_, std::format("field {}: {}", no, message)).error();
}

sl::result<void> struct_reader::done() const {
  if (error_) {
    return std::unexpected(*error_);
  }
  for (const field &f : in_->fields()) {
    if (std::ranges::find(known_, f.no) == known_.end()) {
      return mismatch(type_, std::format("field {} is not declared", f.no));
    }
  }
  return {};
}

} // namespace detail

} // namespace sl::fabric::gen
'''

if __name__ == "__main__":
    sys.exit(main(sys.argv))
