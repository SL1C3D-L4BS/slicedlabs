// RT-INV-REFL-05 (SL-ARCH-003 ARCH3-R135, ADR-0027, ARCH1-R62, `immutability` type registry): a
// binding with a mismatched field number or kind is component_schema_mismatch, a transient type
// (one that binds no field) has no schema type and cannot bind, and registration after freeze is
// type_registry_frozen (off the owner thread wrong_thread). A good binding converts to and from a
// Fabric struct value and names each bound field's path.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace fab = sl::fabric;
using rt::field_flags;

constexpr std::string_view body_type = "test.refl.Body";

struct body {
  float mass = 0.0F;
  std::uint32_t count = 0;
  bool alive = false;
  fab::record_id owner;
  std::int16_t offset = 0;
  double length = 0.0;
  std::uint8_t level = 0;
  std::int32_t cache = 0; // transient, unbound
};

fab::field_desc fld(std::uint32_t no, std::string_view name, std::string_view type) {
  fab::field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = fab::type_id_of(type);
  return f;
}

fab::type_desc named(std::string_view name, fab::type_kind kind) {
  fab::type_desc t;
  t.id = fab::type_id_of(name);
  t.name = std::string(name);
  t.kind = kind;
  return t;
}

void must(const sl::result<void> &r) {
  if (!r) {
    FAIL(r.error().message);
  }
}

fab::schema_registry schemas(bool freeze = true) {
  fab::schema_registry reg;
  fab::type_desc meters = named("test.refl.Meters", fab::type_kind::tagged);
  meters.element = fab::type_id_of("f64");
  fab::type_desc vec = named("test.refl.Vec", fab::type_kind::struct_);
  vec.fields = {fld(1, "x", "f32")};
  fab::type_desc b = named(body_type, fab::type_kind::struct_);
  b.domain = "test.refl";
  b.record_class = fab::id_class::entity;
  b.state_class = fab::state_class::document;
  fab::field_desc level = fld(7, "level", "u8");
  level.presence = fab::presence::optional;
  level.default_value = fab::value::uint(3);
  fab::field_desc offset = fld(5, "offset", "i16");
  offset.presence = fab::presence::optional;
  b.fields = {fld(1, "mass", "f32"),
              fld(2, "count", "u32"),
              fld(3, "alive", "bool"),
              fld(4, "owner", "id"),
              offset,
              fld(6, "length", "test.refl.Meters"),
              level,
              fld(8, "name", "string")};
  for (fab::type_desc t : {meters, vec, b}) {
    must(reg.add(std::move(t)));
  }
  if (freeze) {
    must(reg.freeze());
  }
  return reg;
}

rt::type_builder<body> body_builder() {
  rt::type_builder<body> b(body_type);
  SL_RT_FIELD_BOUND(b, body, mass, field_flags::snapshot | field_flags::hashed, 1);
  SL_RT_FIELD_BOUND(b, body, count, field_flags::snapshot, 2);
  SL_RT_FIELD_BOUND(b, body, alive, field_flags::snapshot, 3);
  SL_RT_FIELD_BOUND(b, body, owner, field_flags::snapshot, 4);
  SL_RT_FIELD_BOUND(b, body, offset, field_flags::snapshot, 5);
  SL_RT_FIELD_BOUND(b, body, length, field_flags::snapshot, 6);
  SL_RT_FIELD_BOUND(b, body, level, field_flags::snapshot, 7);
  SL_RT_FIELD(b, body, cache, field_flags::transient);
  b.schema(body_type);
  return b;
}

template <class T> bool fails(const sl::result<T> &r, sl::errc want) {
  return !r.has_value() && r.error().code == want;
}

body sample() {
  body v;
  v.mass = 2.5F;
  v.count = 42;
  v.alive = true;
  v.owner = fab::well_known(fab::id_class::entity, 11);
  v.offset = -300;
  v.length = 12.75;
  v.level = 9;
  v.cache = 77;
  return v;
}

} // namespace

TEST_CASE("RT-INV-REFL-05: a matching binding converts to and from a Fabric struct value") {
  const fab::schema_registry reg = schemas();
  rt::type_registry types;
  const auto t = types.add(body_builder().build());
  REQUIRE(t.has_value());
  const auto b = rt::fabric_binding::bind(**t, reg, body_type);
  REQUIRE(b.has_value());
  CHECK(b->fields().size() == 7);
  CHECK(b->type() == *t);
  CHECK(b->fabric_type() == fab::type_id_of(body_type));
  const body in = sample();
  const auto v = b->to_value(&in);
  REQUIRE(v.has_value());
  CHECK(v->kind() == fab::value_kind::struct_);
  CHECK(v->fields().size() == 7);
  REQUIRE(v->find_field(2) != nullptr);
  CHECK(v->find_field(2)->as_uint() == 42);
  REQUIRE(v->find_field(5) != nullptr);
  CHECK(v->find_field(5)->as_i64() == -300);
  body out;
  out.cache = 5;
  REQUIRE(b->from_value(*v, &out).has_value());
  CHECK(out.mass == in.mass);
  CHECK(out.count == in.count);
  CHECK(out.alive == in.alive);
  CHECK(out.owner == in.owner);
  CHECK(out.offset == in.offset);
  CHECK(out.length == in.length);
  CHECK(out.level == in.level);
  CHECK(out.cache == 5);
  const fab::path p = b->path_of(4);
  REQUIRE(p.size() == 1);
  CHECK(p[0] == fab::path_segment::field(4));
  CHECK(b->path_of(8).empty());
}

TEST_CASE("RT-INV-REFL-05: absent fields take defaults; bad values are invalid_argument") {
  const fab::schema_registry reg = schemas();
  rt::type_registry types;
  const auto t = types.add(body_builder().build());
  REQUIRE(t.has_value());
  const auto b = rt::fabric_binding::bind(**t, reg, body_type);
  REQUIRE(b.has_value());
  const body in = sample();
  const auto full = b->to_value(&in);
  REQUIRE(full.has_value());
  const auto no_level = full->without(fab::path{fab::path_segment::field(7)});
  REQUIRE(no_level.has_value());
  const auto no_offset = no_level->without(fab::path{fab::path_segment::field(5)});
  REQUIRE(no_offset.has_value());
  body out = sample();
  REQUIRE(b->from_value(*no_offset, &out).has_value());
  CHECK(out.level == 3);  // the Fabric default
  CHECK(out.offset == 0); // optional, no default
  const auto no_mass = full->without(fab::path{fab::path_segment::field(1)});
  REQUIRE(no_mass.has_value());
  body untouched = sample();
  CHECK(fails(b->from_value(*no_mass, &untouched), sl::errc::invalid_argument));
  const auto wide = full->with(fab::path{fab::path_segment::field(5)}, fab::value::integer(40000));
  REQUIRE(wide.has_value());
  CHECK(fails(b->from_value(*wide, &untouched), sl::errc::invalid_argument));
  const auto wrong_kind = full->with(fab::path{fab::path_segment::field(3)}, fab::value::uint(1));
  REQUIRE(wrong_kind.has_value());
  CHECK(fails(b->from_value(*wrong_kind, &untouched), sl::errc::invalid_argument));
  CHECK(fails(b->from_value(fab::value::uint(1), &untouched), sl::errc::invalid_argument));
  CHECK(untouched.mass == in.mass);
  CHECK(untouched.offset == in.offset);
  body nan = sample();
  nan.mass = std::numeric_limits<float>::quiet_NaN();
  CHECK(fails(b->to_value(&nan), sl::errc::invalid_argument));
}

TEST_CASE("RT-INV-REFL-05: a mismatched field number or kind is component_schema_mismatch") {
  const fab::schema_registry reg = schemas();
  const auto bind = [&reg](const rt::type_info &info, std::string_view as) {
    return rt::fabric_binding::bind(info, reg, as);
  };
  const rt::type_info good = body_builder().build();
  rt::type_info t = good;
  t.fields[1].fabric_field = 9; // count -> no field 9
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.fields[0].fabric_field = 2; // mass (f32) -> count (u32)
  t.fields[1].fabric_field = 1; // count (u32) -> mass (f32)
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.fields[4].kind = rt::field_kind::u16; // offset is i16 in Fabric
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.fields[1].kind = rt::field_kind::u64; // a width mismatch
  t.fields[1].size = 8;
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.fields[6].fabric_field = 8; // level (u8) -> name (string)
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.fields[3].kind = rt::field_kind::entity; // entities never bind
  t.fields[3].size = 8;
  CHECK(fails(bind(t, body_type), sl::errc::component_schema_mismatch));
  t = good;
  t.schema_type = "test.refl.Vec"; // not a record type
  CHECK(fails(bind(t, "test.refl.Vec"), sl::errc::component_schema_mismatch));
  t.schema_type = "test.refl.Missing";
  CHECK(fails(bind(t, "test.refl.Missing"), sl::errc::component_schema_mismatch));
  CHECK(fails(bind(good, "test.refl.Vec"), sl::errc::component_schema_mismatch));
  const fab::schema_registry open = schemas(false);
  CHECK(fails(rt::fabric_binding::bind(good, open, body_type), sl::errc::invalid_argument));
}

TEST_CASE("RT-INV-REFL-05: a transient type has no schema type and cannot bind") {
  const fab::schema_registry reg = schemas();
  rt::type_registry types;
  rt::type_builder<body> transient("test.refl.Transient");
  SL_RT_FIELD(transient, body, mass, field_flags::snapshot);
  SL_RT_FIELD(transient, body, cache, field_flags::transient);
  const auto plain = types.add(transient.build());
  REQUIRE(plain.has_value());
  CHECK_FALSE((*plain)->schema_type.has_value());
  CHECK(fails(rt::fabric_binding::bind(**plain, reg, body_type), sl::errc::invalid_argument));
  transient.schema(body_type); // binds no field, so it is transient: no schema type allowed
  rt::type_info named_transient = transient.build();
  named_transient.name = "test.refl.Transient2";
  CHECK(fails(types.add(named_transient), sl::errc::invalid_argument));
  rt::type_info bound_transient = body_builder().build();
  bound_transient.name = "test.refl.Body2";
  bound_transient.fields[7].fabric_field = 8; // a transient field bound to a Fabric field
  CHECK(fails(types.add(bound_transient), sl::errc::invalid_argument));
  rt::type_info unnamed = body_builder().build();
  unnamed.name = "test.refl.Body3";
  unnamed.schema_type.reset(); // Fabric numbers without a schema type
  CHECK(fails(types.add(unnamed), sl::errc::invalid_argument));
}

void add_from_other_thread(rt::type_registry &types, sl::result<const rt::type_info *> &out) {
  rt::type_info t = body_builder().build();
  t.name = "test.refl.Other";
  out = types.add(t);
}

TEST_CASE("RT-INV-REFL-05: registration after freeze is type_registry_frozen") {
  rt::type_registry types;
  REQUIRE(types.add(body_builder().build()).has_value());
  sl::result<const rt::type_info *> off_thread = nullptr;
  std::thread other(add_from_other_thread, std::ref(types), std::ref(off_thread));
  other.join();
  CHECK(fails(off_thread, sl::errc::wrong_thread));
  CHECK_FALSE(types.frozen());
  REQUIRE(types.freeze().has_value());
  REQUIRE(types.freeze().has_value());
  CHECK(types.frozen());
  rt::type_info late = body_builder().build();
  late.name = "test.refl.Late";
  CHECK(fails(types.add(late), sl::errc::type_registry_frozen));
  CHECK(types.size() == 1);
  CHECK(types.find(body_type).has_value());
}
