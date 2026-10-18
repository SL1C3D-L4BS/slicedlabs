"""Schema compatibility classifier (WO-040, ARCH1-R21, SL-ARCH-001 `compatibility`). Stdlib only (ADR-0004).

  classify(old, new)   -> identical | additive | backward_only | breaking | forbidden
  changes(old, new)    -> [Change(code, cls, detail)]  every difference, sorted; classify is the worst class
  required_bump(cls)   -> (version bump, migration): identical (none, none), additive (minor, none),
                          backward_only (major, identity), breaking (major, step), forbidden (refused, none)
  given_bump(old_version, new_version) -> none | minor | major | invalid

`old` and `new` are the same type (same name) in module form or normalized form (schemalib.normalize). Fields are
matched by number, enumerators and cases by number; names, docs and since_minor are labels. ROWS maps each row of
the SL-ARCH-001 `compatibility` table to the change code that produces it; tests/fixtures/fabric/schema/
compat_table.json holds one {old, new, expected} pair per row (WO-057 reads it from C++). Extra codes beyond the
table: remove_unreserved (a removed field number must be reserved: ARCH1-R13), change_document_root (breaking).
"""

import os
import sys
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schemalib  # noqa: E402

CLASSES = ("identical", "additive", "backward_only", "breaking", "forbidden")
RANK = {c: i for i, c in enumerate(CLASSES)}
BUMPS = ("none", "minor", "major")
BUMP_RANK = {b: i for i, b in enumerate(BUMPS)}
REQUIRED = {
    "identical": ("none", "none"),
    "additive": ("minor", "none"),
    "backward_only": ("major", "identity"),
    "breaking": ("major", "step"),
    "forbidden": ("refused", "none"),
}

# SL-ARCH-001 `compatibility` change text -> (code, class)
ROWS = {
    "rename field label or edit doc": ("label", "identical"),
    "deprecate field": ("deprecate", "identical"),
    "add optional field": ("add_optional_field", "additive"),
    "add enum value": ("add_enum_value", "additive"),
    "add variant case": ("add_variant_case", "additive"),
    "add ref target type": ("add_ref_target", "additive"),
    "ref strength strong to weak": ("ref_strong_to_weak", "additive"),
    "remove optional field (number reserved)": ("remove_optional_field", "backward_only"),
    "required to optional": ("required_to_optional", "backward_only"),
    "widen integer range": ("widen_int", "backward_only"),
    "add required field": ("add_required_field", "breaking"),
    "remove required field": ("remove_required_field", "breaking"),
    "change field type": ("change_field_type", "breaking"),
    "narrow integer range": ("narrow_int", "breaking"),
    "remove or renumber enum value": ("remove_enum_value", "breaking"),
    "remove variant case": ("remove_variant_case", "breaking"),
    "change default of optional field": ("change_default", "breaking"),
    "optional to required": ("optional_to_required", "breaking"),
    "ref strength weak to strong": ("ref_weak_to_strong", "breaking"),
    "remove ref target type": ("remove_ref_target", "breaking"),
    "list to oset or oset to list": ("list_oset_swap", "breaking"),
    "change domain, state class or record class": ("change_domain", "forbidden"),
    "reuse a reserved field number": ("reuse_reserved", "forbidden"),
}
# codes that also belong to the "change domain, state class or record class" row, and the extras
CODE_CLASS = {code: cls for code, cls in ROWS.values()}
CODE_CLASS.update({
    "change_state_class": "forbidden", "change_record_class": "forbidden",
    "remove_unreserved": "forbidden", "change_document_root": "breaking",
})

Change = namedtuple("Change", "code cls detail")


def _c(code, detail):
    return Change(code, CODE_CLASS[code], detail)


def _int_change(old_t, new_t):
    """Code for a change between two type names: widen_int, narrow_int or change_field_type."""
    if old_t in schemalib.INT_RANGES and new_t in schemalib.INT_RANGES:
        (ol, oh), (nl, nh) = schemalib.INT_RANGES[old_t], schemalib.INT_RANGES[new_t]
        if nl <= ol and nh >= oh:
            return "widen_int"
        if nl >= ol and nh <= oh:
            return "narrow_int"
    return "change_field_type"


def _field_changes(old, new, out, label):
    if old["type"] != new["type"]:
        out.append(_c(_int_change(old["type"], new["type"]),
                      "%s type %s -> %s" % (label, old["type"], new["type"])))
    elif old["type"] == schemalib.REF_TYPE:
        _ref_changes(old.get("ref"), new.get("ref"), out, label)
    if old["presence"] != new["presence"]:
        code = "required_to_optional" if old["presence"] == "required" else "optional_to_required"
        out.append(_c(code, "%s presence %s -> %s" % (label, old["presence"], new["presence"])))
    if old.get("default", _MISSING) != new.get("default", _MISSING):
        out.append(_c("change_default", "%s default %r -> %r" % (label, old.get("default"), new.get("default"))))
    if old.get("deprecated_in") != new.get("deprecated_in"):
        out.append(_c("deprecate", "%s deprecated_in %r -> %r" % (label, old.get("deprecated_in"),
                                                                  new.get("deprecated_in"))))
    for key in ("name", "doc", "since_minor"):
        if old[key] != new[key]:
            out.append(_c("label", "%s %s changed" % (label, key)))


_MISSING = object()


def _ref_changes(old, new, out, label):
    if old is None or new is None:
        out.append(_c("change_field_type", "%s ref declaration appeared or vanished" % label))
        return
    if old["strength"] != new["strength"]:
        pair = (old["strength"], new["strength"])
        code = "ref_strong_to_weak" if pair == ("strong", "weak") else "ref_weak_to_strong" \
            if pair == ("weak", "strong") else "change_field_type"
        out.append(_c(code, "%s ref strength %s -> %s" % (label, pair[0], pair[1])))
    for target in sorted(set(new["targets"]) - set(old["targets"])):
        out.append(_c("add_ref_target", "%s ref target %s added" % (label, target)))
    for target in sorted(set(old["targets"]) - set(new["targets"])):
        out.append(_c("remove_ref_target", "%s ref target %s removed" % (label, target)))


def _members(old, new, out, kind, add_code, remove_code):
    """Enumerators or cases, matched by number."""
    olds, news = {m["no"]: m for m in old}, {m["no"]: m for m in new}
    for no in sorted(set(news) - set(olds)):
        out.append(_c(add_code, "%s %d (%s) added" % (kind, no, news[no]["name"])))
    for no in sorted(set(olds) - set(news)):
        out.append(_c(remove_code, "%s %d (%s) removed" % (kind, no, olds[no]["name"])))
    for no in sorted(set(olds) & set(news)):
        o, n = olds[no], news[no]
        label = "%s %d" % (kind, no)
        if o.get("type") != n.get("type"):
            out.append(_c(_int_change(o.get("type"), n.get("type")), "%s type %s -> %s" % (label, o.get("type"),
                                                                                         n.get("type"))))
        if o.get("deprecated_in") != n.get("deprecated_in"):
            out.append(_c("deprecate", "%s deprecated_in changed" % label))
        for key in ("name", "doc", "since_minor"):
            if o[key] != n[key]:
                out.append(_c("label", "%s %s changed" % (label, key)))


def _struct_changes(old, new, out):
    olds, news = {f["no"]: f for f in old["fields"]}, {f["no"]: f for f in new["fields"]}
    reserved_before, reserved_after = set(old["reserved"]), set(new["reserved"])
    for no in sorted(set(news) - set(olds)):
        f = news[no]
        if no in reserved_before or no in reserved_after:
            out.append(_c("reuse_reserved", "field %d (%s) uses a reserved number" % (no, f["name"])))
        code = "add_optional_field" if f["presence"] == "optional" else "add_required_field"
        out.append(_c(code, "field %d (%s) added" % (no, f["name"])))
    for no in sorted(set(olds) - set(news)):
        f = olds[no]
        if no not in reserved_after:
            out.append(_c("remove_unreserved",
                          "field %d (%s) removed but its number is not reserved" % (no, f["name"])))
        code = "remove_optional_field" if f["presence"] == "optional" else "remove_required_field"
        out.append(_c(code, "field %d (%s) removed" % (no, f["name"])))
    for no in sorted(set(olds) & set(news)):
        _field_changes(olds[no], news[no], out, "field %d (%s)" % (no, news[no]["name"]))
    for no in sorted(reserved_before - reserved_after):
        out.append(_c("reuse_reserved", "reserved number %d was dropped from the reserved list" % no))
    for no in sorted(reserved_after & set(news)):
        if no in olds:
            out.append(_c("reuse_reserved", "field number %d is both a field and reserved" % no))


def changes(old, new):
    """Every difference between two versions of one type, as a sorted list of Change."""
    o, n = schemalib.normalize(old), schemalib.normalize(new)
    if o["name"] != n["name"]:
        raise ValueError("classify compares two versions of one type: %s vs %s" % (o["name"], n["name"]))
    out = []
    for key, code in (("domain", "change_domain"), ("state_class", "change_state_class"),
                      ("record_class", "change_record_class"), ("document_root", "change_document_root")):
        if o[key] != n[key]:
            out.append(_c(code, "%s %r -> %r" % (key, o[key], n[key])))
    if o["kind"] != n["kind"]:
        swap = {o["kind"], n["kind"]} == {"list", "oset"}
        out.append(_c("list_oset_swap" if swap else "change_field_type", "kind %s -> %s" % (o["kind"], n["kind"])))
    else:
        kind = o["kind"]
        if kind == "struct":
            _struct_changes(o, n, out)
        elif kind == "enum":
            _members(o["enumerators"], n["enumerators"], out, "enumerator", "add_enum_value", "remove_enum_value")
        elif kind == "variant":
            _members(o["cases"], n["cases"], out, "case", "add_variant_case", "remove_variant_case")
        for key in ("element", "key"):
            if o.get(key) != n.get(key):
                out.append(_c(_int_change(o.get(key), n.get(key)), "%s %s -> %s" % (key, o.get(key), n.get(key))))
    return sorted(out, key=lambda c: (-RANK[c.cls], c.code, c.detail))


def classify(old, new):
    """The worst class over all differences; identical when there are none."""
    found = changes(old, new)
    return found[0].cls if found else "identical"


def required_bump(cls):
    """(version bump, migration) that a class demands (ARCH1-R21)."""
    return REQUIRED[cls]


def given_bump(old_version, new_version):
    """none | minor | major for "major.minor" strings; invalid when the new version is lower."""
    (om, on), (nm, nn) = schemalib.split_version(old_version), schemalib.split_version(new_version)
    if (nm, nn) == (om, on):
        return "none"
    if nm > om:
        return "major"
    if nm == om and nn > on:
        return "minor"
    return "invalid"


def bump_satisfies(given, required):
    """True when the given bump is at least the required one (forbidden is never satisfied)."""
    return required != "refused" and given != "invalid" and BUMP_RANK[given] >= BUMP_RANK[required]
