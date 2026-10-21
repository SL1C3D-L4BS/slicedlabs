// RT-INV-REFL-01 (SL-ARCH-003 ARCH3-R133, `limits` max_fields_per_type): the field metadata the
// builder and the registry record (offset, size, kind) equals offsetof and sizeof for every
// reference type, the type records sizeof, alignof and the FNV-1a-64 key of its name, and the
// registry refuses malformed layouts and more than 64 fields.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rt = sl::runtime;
using rt::field_flags;
using rt::field_kind;

struct ref_scalars {
  bool b = false;
  std::int8_t i8 = 0;
  std::int16_t i16 = 0;
  std::int32_t i32 = 0;
  std::int64_t i64 = 0;
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  float f32 = 0.0F;
  double f64 = 0.0;
};

struct ref_handles {
  rt::entity_bits e;
  sl::fabric::record_id id;
  std::array<std::byte, 5> blob{};
  std::uint8_t tail = 0;
  std::array<std::uint8_t, 3> raw{};
};

struct ref_padded {
  std::uint8_t a = 0;
  std::uint32_t b = 0;
  std::uint8_t c = 0;
  double d = 0.0;
  std::uint16_t e = 0;
};

constexpr field_flags all = field_flags::snapshot | field_flags::hashed;

rt::type_info scalars_info() {
  rt::type_builder<ref_scalars> b("test.refl.scalars");
  SL_RT_FIELD(b, ref_scalars, b, all);
  SL_RT_FIELD(b, ref_scalars, i8, all);
  SL_RT_FIELD(b, ref_scalars, i16, all);
  SL_RT_FIELD(b, ref_scalars, i32, all);
  SL_RT_FIELD(b, ref_scalars, i64, all);
  SL_RT_FIELD(b, ref_scalars, u8, all);
  SL_RT_FIELD(b, ref_scalars, u16, all);
  SL_RT_FIELD(b, ref_scalars, u32, all);
  SL_RT_FIELD(b, ref_scalars, u64, all);
  SL_RT_FIELD(b, ref_scalars, f32, all);
  SL_RT_FIELD(b, ref_scalars, f64, all);
  return b.build();
}

rt::type_info handles_info() {
  rt::type_builder<ref_handles> b("test.refl.handles");
  SL_RT_FIELD(b, ref_handles, e, all);
  SL_RT_FIELD(b, ref_handles, id, all);
  SL_RT_FIELD(b, ref_handles, blob, all);
  SL_RT_FIELD(b, ref_handles, tail, field_flags::transient);
  SL_RT_FIELD(b, ref_handles, raw, field_flags::snapshot);
  return b.build();
}

rt::type_info padded_info() {
  rt::type_builder<ref_padded> b("test.refl.padded");
  SL_RT_FIELD(b, ref_padded, a, all);
  SL_RT_FIELD(b, ref_padded, b, all);
  SL_RT_FIELD(b, ref_padded, c, all);
  SL_RT_FIELD(b, ref_padded, d, all);
  SL_RT_FIELD(b, ref_padded, e, all);
  return b.build();
}

struct expect_field {
  std::string_view name;
  std::size_t offset = 0;
  std::size_t size = 0;
  field_kind kind = field_kind::boolean;
};

expect_field ef(std::string_view name, std::size_t offset, std::size_t size, field_kind kind) {
  return expect_field{.name = name, .offset = offset, .size = size, .kind = kind};
}

void check_fields(const rt::type_info &t, const std::vector<expect_field> &want) {
  REQUIRE(t.fields.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    CAPTURE(want[i].name);
    CHECK(t.fields[i].name == want[i].name);
    CHECK(t.fields[i].offset == want[i].offset);
    CHECK(t.fields[i].size == want[i].size);
    CHECK(t.fields[i].kind == want[i].kind);
  }
}

std::vector<expect_field> scalars_want() {
  return {
      ef("b", offsetof(ref_scalars, b), sizeof(bool), field_kind::boolean),
      ef("i8", offsetof(ref_scalars, i8), sizeof(std::int8_t), field_kind::i8),
      ef("i16", offsetof(ref_scalars, i16), sizeof(std::int16_t), field_kind::i16),
      ef("i32", offsetof(ref_scalars, i32), sizeof(std::int32_t), field_kind::i32),
      ef("i64", offsetof(ref_scalars, i64), sizeof(std::int64_t), field_kind::i64),
      ef("u8", offsetof(ref_scalars, u8), sizeof(std::uint8_t), field_kind::u8),
      ef("u16", offsetof(ref_scalars, u16), sizeof(std::uint16_t), field_kind::u16),
      ef("u32", offsetof(ref_scalars, u32), sizeof(std::uint32_t), field_kind::u32),
      ef("u64", offsetof(ref_scalars, u64), sizeof(std::uint64_t), field_kind::u64),
      ef("f32", offsetof(ref_scalars, f32), sizeof(float), field_kind::f32),
      ef("f64", offsetof(ref_scalars, f64), sizeof(double), field_kind::f64),
  };
}

std::vector<expect_field> handles_want() {
  return {
      ef("e", offsetof(ref_handles, e), sizeof(rt::entity_bits), field_kind::entity),
      ef("id", offsetof(ref_handles, id), sizeof(sl::fabric::record_id), field_kind::record_id),
      ef("blob", offsetof(ref_handles, blob), sizeof(std::array<std::byte, 5>),
         field_kind::bytes_fixed),
      ef("tail", offsetof(ref_handles, tail), sizeof(std::uint8_t), field_kind::u8),
      ef("raw", offsetof(ref_handles, raw), sizeof(std::array<std::uint8_t, 3>),
         field_kind::bytes_fixed),
  };
}

std::vector<expect_field> padded_want() {
  return {
      ef("a", offsetof(ref_padded, a), sizeof(std::uint8_t), field_kind::u8),
      ef("b", offsetof(ref_padded, b), sizeof(std::uint32_t), field_kind::u32),
      ef("c", offsetof(ref_padded, c), sizeof(std::uint8_t), field_kind::u8),
      ef("d", offsetof(ref_padded, d), sizeof(double), field_kind::f64),
      ef("e", offsetof(ref_padded, e), sizeof(std::uint16_t), field_kind::u16),
  };
}

} // namespace

TEST_CASE("RT-INV-REFL-01: builder metadata equals offsetof and sizeof for every reference type") {
  const rt::type_info s = scalars_info();
  CHECK(s.size == sizeof(ref_scalars));
  CHECK(s.align == alignof(ref_scalars));
  check_fields(s, scalars_want());
  const rt::type_info h = handles_info();
  CHECK(h.size == sizeof(ref_handles));
  CHECK(h.align == alignof(ref_handles));
  check_fields(h, handles_want());
  CHECK(h.fields[3].flags == field_flags::transient);
  CHECK(h.fields[4].flags == field_flags::snapshot);
  const rt::type_info p = padded_info();
  CHECK(p.size == sizeof(ref_padded));
  CHECK(p.align == alignof(ref_padded));
  check_fields(p, padded_want());
  CHECK(rt::kind_size(field_kind::entity) == sizeof(rt::entity_bits));
  CHECK(rt::kind_size(field_kind::record_id) == sizeof(sl::fabric::record_id));
}

TEST_CASE("RT-INV-REFL-01: the registry keeps the metadata, owns the names and sets the key") {
  rt::type_registry reg;
  std::string name = "test.refl.owned";
  std::string field_name = "value";
  rt::type_info t;
  t.name = name;
  t.size = 8;
  t.align = 4;
  t.fields.push_back(rt::field_info{.name = field_name,
                                    .offset = 4,
                                    .size = 4,
                                    .kind = field_kind::f32,
                                    .flags = field_flags::hashed});
  t.type_key = 1; // add() replaces it
  const sl::result<const rt::type_info *> added = reg.add(t);
  REQUIRE(added.has_value());
  name.assign("overwritten-name");
  field_name.assign("overwritten");
  const sl::result<const rt::type_info *> found = reg.find("test.refl.owned");
  REQUIRE(found.has_value());
  CHECK(*found == *added);
  CHECK((*found)->name == "test.refl.owned");
  CHECK((*found)->fields[0].name == "value");
  CHECK((*found)->fields[0].offset == 4);
  CHECK((*found)->type_key == rt::type_key_of("test.refl.owned"));
  REQUIRE(reg.add(scalars_info()).has_value());
  REQUIRE(reg.add(handles_info()).has_value());
  REQUIRE(reg.add(padded_info()).has_value());
  const auto s = reg.find("test.refl.scalars");
  REQUIRE(s.has_value());
  check_fields(**s, scalars_want());
  const auto h = reg.find("test.refl.handles");
  REQUIRE(h.has_value());
  check_fields(**h, handles_want());
  const auto p = reg.find("test.refl.padded");
  REQUIRE(p.has_value());
  check_fields(**p, padded_want());
}

TEST_CASE("RT-INV-REFL-01: FNV-1a-64 type keys") {
  static_assert(rt::type_key_of("") == 0xcbf29ce484222325ULL);
  static_assert(rt::type_key_of("a") == 0xaf63dc4c8601ec8cULL);
  static_assert(rt::type_key_of("foobar") == 0x85944171f73967e8ULL);
  CHECK(scalars_info().type_key == rt::type_key_of("test.refl.scalars"));
}

TEST_CASE("RT-INV-REFL-01: malformed layouts and more than 64 fields are invalid_argument") {
  struct wide {
    std::array<std::uint8_t, 65> bytes{};
  };
  std::vector<std::string> names;
  names.reserve(65);
  for (int i = 0; i < 65; ++i) {
    names.push_back("f" + std::to_string(i));
  }
  rt::type_builder<wide> b64("test.refl.wide64");
  rt::type_builder<wide> b65("test.refl.wide65");
  for (std::uint32_t i = 0; i < 65; ++i) {
    if (i < 64) {
      b64.field(names[i], i, field_kind::u8, field_flags::hashed);
    }
    b65.field(names[i], i, field_kind::u8, field_flags::hashed);
  }
  rt::type_registry reg;
  CHECK(reg.add(b64.build()).has_value());
  const auto too_many = reg.add(b65.build());
  REQUIRE_FALSE(too_many.has_value());
  CHECK(too_many.error().code == sl::errc::invalid_argument);

  const auto refused = [&reg](const rt::type_info &t) {
    const auto r = reg.add(t);
    return !r.has_value() && r.error().code == sl::errc::invalid_argument;
  };
  rt::type_info base = padded_info();
  base.name = "test.refl.bad";
  rt::type_info t = base;
  t.fields[1].offset = 0; // overlaps a
  CHECK(refused(t));
  t = base;
  std::swap(t.fields[0], t.fields[1]); // out of declaration order
  CHECK(refused(t));
  t = base;
  t.fields[4].offset = t.size - 1; // past the end
  CHECK(refused(t));
  t = base;
  t.fields[1].size = 2; // size does not match u32
  CHECK(refused(t));
  t = base;
  t.fields[2].name = "a"; // repeated name
  CHECK(refused(t));
  t = base;
  t.fields[0].flags = field_flags::transient | field_flags::hashed;
  CHECK(refused(t));
  t = base;
  t.align = 3;
  CHECK(refused(t));
  t = base;
  t.name = "";
  CHECK(refused(t));
  CHECK(reg.add(base).has_value());
  const auto again = reg.add(base);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == sl::errc::already_exists);
}
