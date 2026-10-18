// FAB-INV-TY-01 (SL-ARCH-001 ARCH1-R13): a type_desc is well formed: unique field numbers, names,
// enumerators and cases; reserved numbers unused; recursion only through an optional field, a
// list, a map or an oset.

#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

type_desc named(std::string_view name, type_kind kind) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  return t;
}

field_desc fld(std::uint32_t no, std::string_view name, std::string_view type,
               presence p = presence::required) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  f.presence = p;
  return f;
}

enumerator_desc enumerator(std::uint32_t no, std::string_view name) {
  enumerator_desc e;
  e.no = no;
  e.name = std::string(name);
  return e;
}

case_desc variant_case(std::uint32_t no, std::string_view name, std::string_view type) {
  case_desc c;
  c.no = no;
  c.name = std::string(name);
  c.type = type_id_of(type);
  return c;
}

/// The rule id that opens a schema_invalid message, or "ok".
std::string rule_of(const sl::result<void> &r) {
  if (r) {
    return "ok";
  }
  CHECK(r.error().code == sl::errc::schema_invalid);
  const std::string &m = r.error().message;
  return m.substr(0, m.find(':'));
}

/// Adds every type, then freezes; the freeze result (adds must succeed).
sl::result<void> freeze_set(std::vector<type_desc> types) {
  schema_registry reg;
  for (type_desc &t : types) {
    const auto added = reg.add(std::move(t));
    REQUIRE_MESSAGE(added.has_value(), (added ? std::string() : added.error().message));
  }
  return reg.freeze();
}

bool mentions(const sl::result<void> &r, std::string_view text) {
  return !r && r.error().message.contains(text);
}

} // namespace

TEST_CASE("FAB-INV-TY-01: built-in types and the test module are well formed") {
  for (const type_desc &t : builtin_types()) {
    CAPTURE(t.name);
    CHECK(check_well_formed(t).has_value());
    CHECK(t.id == type_id_of(t.name));
    CHECK(is_builtin_type(t.id));
  }
  CHECK(builtin_types().size() == 18);
  // Type ids are derived ids of class type from the project root namespace (ARCH1-R12).
  const record_id root = well_known(id_class::namespace_, 1);
  CHECK(type_id_of("content.scene.Entity").id() ==
        derive(id_class::type, root, "content.scene.Entity"));
  CHECK(type_id_of("u32").id().scheme() == mint_scheme::derived);
  const auto reg = testing::make_test_registry();
  REQUIRE_MESSAGE(reg.has_value(), (reg ? std::string() : reg.error().message));
  CHECK(reg->frozen());
  CHECK(reg->find(testing::test_types::entity) != nullptr);
}

TEST_CASE("FAB-INV-TY-01: field numbers and names are unique") {
  type_desc t = named("t.S", type_kind::struct_);
  t.fields = {fld(1, "a", "u32"), fld(2, "b", "u32")};
  CHECK(rule_of(check_well_formed(t)) == "ok");

  type_desc dup_no = t;
  dup_no.fields[1].no = 1;
  CHECK(rule_of(check_well_formed(dup_no)) == "R-TY-FIELD");

  type_desc dup_name = t;
  dup_name.fields[1].name = "a";
  CHECK(rule_of(check_well_formed(dup_name)) == "R-TY-FIELD");

  type_desc zero = t;
  zero.fields[0].no = 0;
  CHECK(rule_of(check_well_formed(zero)) == "R-TY-FIELD");

  type_desc untyped = t;
  untyped.fields[0].type = type_id{};
  CHECK(rule_of(check_well_formed(untyped)) == "R-TY-FIELD");

  // The registry refuses the type itself, not just a later freeze.
  schema_registry reg;
  CHECK(rule_of(reg.add(dup_no)) == "R-TY-FIELD");
  CHECK(reg.find("t.S") == nullptr);
}

TEST_CASE("FAB-INV-TY-01: enumerators and cases are unique") {
  type_desc e = named("t.E", type_kind::enum_);
  e.enumerators = {enumerator(0, "zero"), enumerator(1, "one")};
  CHECK(rule_of(check_well_formed(e)) == "ok");
  type_desc e_no = e;
  e_no.enumerators[1].no = 0;
  CHECK(rule_of(check_well_formed(e_no)) == "R-TY-ENUM");
  type_desc e_name = e;
  e_name.enumerators[1].name = "zero";
  CHECK(rule_of(check_well_formed(e_name)) == "R-TY-ENUM");
  type_desc e_none = e;
  e_none.enumerators.clear();
  CHECK(rule_of(check_well_formed(e_none)) == "R-TY-ENUM");

  type_desc v = named("t.V", type_kind::variant);
  v.cases = {variant_case(1, "a", "u8"), variant_case(2, "b", "string")};
  CHECK(rule_of(check_well_formed(v)) == "ok");
  type_desc v_no = v;
  v_no.cases[1].no = 1;
  CHECK(rule_of(check_well_formed(v_no)) == "R-TY-CASE");
  type_desc v_name = v;
  v_name.cases[1].name = "a";
  CHECK(rule_of(check_well_formed(v_name)) == "R-TY-CASE");
  type_desc v_none = v;
  v_none.cases.clear();
  CHECK(rule_of(check_well_formed(v_none)) == "R-TY-CASE");
}

TEST_CASE("FAB-INV-TY-01: reserved numbers are unique and used by no field") {
  type_desc t = named("t.R", type_kind::struct_);
  t.fields = {fld(1, "a", "u32"), fld(3, "c", "u32")};
  t.reserved = {2, 4};
  CHECK(rule_of(check_well_formed(t)) == "ok");
  type_desc reused = t;
  reused.fields.push_back(fld(2, "b", "u32"));
  CHECK(rule_of(check_well_formed(reused)) == "R-TY-RESERVED");
  type_desc twice = t;
  twice.reserved = {2, 2};
  CHECK(rule_of(check_well_formed(twice)) == "R-TY-RESERVED");
}

TEST_CASE("FAB-INV-TY-01: names, ids, versions, shapes, defaults, since and refs") {
  type_desc t = named("t.S", type_kind::struct_);
  t.fields = {fld(1, "a", "u32", presence::optional)};

  type_desc bad_name = t;
  bad_name.name = "t..S";
  bad_name.id = type_id_of(bad_name.name);
  CHECK(rule_of(check_well_formed(bad_name)) == "R-TY-NAME");
  type_desc bad_field_name = t;
  bad_field_name.fields[0].name = "9a";
  CHECK(rule_of(check_well_formed(bad_field_name)) == "R-TY-NAME");

  type_desc bad_id = t;
  bad_id.id = type_id_of("t.Other");
  CHECK(rule_of(check_well_formed(bad_id)) == "R-TY-ID");

  type_desc v0 = t;
  v0.version.major = 0;
  CHECK(rule_of(check_well_formed(v0)) == "R-TY-VERSION");

  type_desc stray_element = t;
  stray_element.element = type_id_of("u8");
  CHECK(rule_of(check_well_formed(stray_element)) == "R-TY-SHAPE");
  type_desc list_without = named("t.L", type_kind::list);
  CHECK(rule_of(check_well_formed(list_without)) == "R-TY-SHAPE");
  type_desc map_without_key = named("t.M", type_kind::map);
  map_without_key.element = type_id_of("u8");
  CHECK(rule_of(check_well_formed(map_without_key)) == "R-TY-SHAPE");
  type_desc enum_with_fields = named("t.E", type_kind::enum_);
  enum_with_fields.enumerators = {enumerator(0, "a")};
  enum_with_fields.fields = {fld(1, "a", "u8")};
  CHECK(rule_of(check_well_formed(enum_with_fields)) == "R-TY-SHAPE");
  type_desc user_scalar = named("t.Count", type_kind::u32);
  CHECK(rule_of(check_well_formed(user_scalar)) == "R-TY-SHAPE");

  type_desc required_default = t;
  required_default.fields[0].presence = presence::required;
  required_default.fields[0].default_value = sl::fabric::value::uint(1);
  CHECK(rule_of(check_well_formed(required_default)) == "R-TY-DEFAULT");

  type_desc since = t;
  since.fields[0].since_minor = 1; // version 1.0
  CHECK(rule_of(check_well_formed(since)) == "R-TY-SINCE");
  since.version.minor = 2;
  CHECK(rule_of(check_well_formed(since)) == "ok");
  since.fields[0].deprecated_in = 0;
  CHECK(rule_of(check_well_formed(since)) == "R-TY-SINCE");

  type_desc no_target = t;
  no_target.fields[0].type = type_id_of("ref");
  no_target.fields[0].ref = ref_spec{};
  CHECK(rule_of(check_well_formed(no_target)) == "R-TY-REF");
  no_target.fields[0].ref =
      ref_spec{.strength = ref_strength::weak, .targets = {type_id_of("t.S"), type_id_of("t.S")}};
  CHECK(rule_of(check_well_formed(no_target)) == "R-TY-REF");
}

TEST_CASE("FAB-INV-TY-01: recursion only through an optional field, a list, a map or an oset") {
  const auto self = [](presence p) {
    type_desc t = named("t.Node", type_kind::struct_);
    t.fields = {fld(1, "value", "u32"), fld(2, "next", "t.Node", p)};
    return t;
  };
  CHECK(freeze_set({self(presence::optional)}).has_value());
  const auto required = freeze_set({self(presence::required)});
  CHECK(rule_of(required) == "freeze refused, 1 violation(s)");
  CHECK(mentions(required, "R-TY-RECURSION"));
  CHECK(mentions(required, "t.Node -> t.Node"));

  // Through list, map and oset: allowed.
  for (const type_kind k : {type_kind::list, type_kind::map, type_kind::oset}) {
    type_desc node = named("t.Tree", type_kind::struct_);
    node.fields = {fld(1, "children", "t.Children")};
    type_desc children = named("t.Children", k);
    children.element = type_id_of("t.Tree");
    if (k == type_kind::map) {
      children.key = type_id_of("string");
    }
    CAPTURE(to_string(k));
    const auto r = freeze_set({node, children});
    CHECK_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  }

  // Mutual recursion through required fields, a variant or a tagged base: refused.
  type_desc a = named("t.A", type_kind::struct_);
  a.fields = {fld(1, "b", "t.B")};
  type_desc b = named("t.B", type_kind::struct_);
  b.fields = {fld(1, "a", "t.A")};
  CHECK(mentions(freeze_set({a, b}), "R-TY-RECURSION"));

  type_desc v = named("t.V", type_kind::variant);
  v.cases = {variant_case(1, "leaf", "u8"), variant_case(2, "again", "t.V")};
  CHECK(mentions(freeze_set({v}), "R-TY-RECURSION"));

  type_desc t1 = named("t.T1", type_kind::tagged);
  t1.element = type_id_of("t.T2");
  type_desc t2 = named("t.T2", type_kind::tagged);
  t2.element = type_id_of("t.T1");
  CHECK(mentions(freeze_set({t1, t2}), "R-TY-RECURSION"));

  // A variant that recurses through an oset is fine.
  type_desc tree = named("t.Shape", type_kind::variant);
  tree.cases = {variant_case(1, "leaf", "f32"), variant_case(2, "group", "t.Shapes")};
  type_desc shapes = named("t.Shapes", type_kind::oset);
  shapes.element = type_id_of("t.Shape");
  CHECK(freeze_set({tree, shapes}).has_value());
}

TEST_CASE("FAB-INV-TY-01: freeze resolves references, map keys and ref specs") {
  type_desc s = named("t.S", type_kind::struct_);
  s.fields = {fld(1, "missing", "t.Missing")};
  CHECK(mentions(freeze_set({s}), "R-TY-UNRESOLVED"));

  type_desc m = named("t.M", type_kind::map);
  m.key = type_id_of("f64");
  m.element = type_id_of("u8");
  CHECK(mentions(freeze_set({m}), "R-TY-KEY"));
  for (const std::string_view key : {"string", "i8", "u64", "id"}) {
    type_desc ok = m;
    ok.key = type_id_of(key);
    CAPTURE(key);
    CHECK(freeze_set({ok}).has_value());
  }

  type_desc rec = named("t.Rec", type_kind::struct_);
  rec.record_class = id_class::entity;
  rec.state_class = state_class::document;
  rec.domain = "content.t";
  rec.fields = {fld(1, "name", "string")};

  type_desc holder = named("t.Holder", type_kind::struct_);
  holder.fields = {fld(1, "target", "ref")};
  CHECK(mentions(freeze_set({rec, holder}), "R-TY-REF")); // ref without spec
  holder.fields[0].ref = ref_spec{.strength = ref_strength::strong, .targets = {rec.id}};
  CHECK(freeze_set({rec, holder}).has_value());
  holder.fields[0].ref =
      ref_spec{.strength = ref_strength::strong, .targets = {type_id_of("t.Plain")}};
  type_desc plain = named("t.Plain", type_kind::struct_);
  CHECK(mentions(freeze_set({rec, holder, plain}), "is not a record type"));

  type_desc spec_without_ref = named("t.H2", type_kind::struct_);
  spec_without_ref.fields = {fld(1, "n", "u8")};
  spec_without_ref.fields[0].ref = ref_spec{.strength = ref_strength::weak, .targets = {rec.id}};
  CHECK(mentions(freeze_set({rec, spec_without_ref}), "holds no ref"));

  type_desc refs = named("t.Refs", type_kind::list);
  refs.element = type_id_of("ref");
  type_desc v = named("t.V", type_kind::variant);
  v.cases = {variant_case(1, "r", "t.Refs")};
  CHECK(mentions(freeze_set({refs, v}), "need a field"));
}

TEST_CASE("FAB-INV-TY-01: generated type sets are well formed and freeze") {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    CAPTURE(seed);
    schema_registry reg;
    testing::value_gen gen(seed);
    for (int i = 0; i < 3; ++i) {
      const auto id = gen.add_type(reg);
      REQUIRE_MESSAGE(id.has_value(), (id ? std::string() : id.error().message));
      const type_desc *t = reg.find(*id);
      REQUIRE(t != nullptr);
      CHECK(check_well_formed(*t).has_value());
    }
    const auto frozen = reg.freeze();
    CHECK_MESSAGE(frozen.has_value(), (frozen ? std::string() : frozen.error().message));
  }
}
