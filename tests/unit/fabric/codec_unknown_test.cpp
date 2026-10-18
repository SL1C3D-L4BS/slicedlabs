// FAB-INV-SER-08 (SL-ARCH-001 ARCH1-R28): a struct field or variant case the schema does not
// declare is preserved byte-exact under unknown_policy::preserve (decode, re-encode, text, back)
// and refused with R-SER-UNKNOWN under strict, at any depth.

#include "sl/fabric/codec.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;
namespace tt = testing::test_types;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value str(std::string_view s) { return must(value::string(s)); }

/// A Vec3 with an extra field 9 holding a nested struct (a newer minor's field).
value vec3_with_extra() {
  const value extra = must(
      value::struct_({field{.no = 1, .v = str("future")}, field{.no = 2, .v = value::nint(41)}}));
  return must(value::struct_(
      {field{.no = 1, .v = must(value::f64(1.0))}, field{.no = 2, .v = must(value::f64(2.0))},
       field{.no = 3, .v = must(value::f64(3.0))}, field{.no = 9, .v = extra}}));
}

} // namespace

TEST_CASE("FAB-INV-SER-08: unknown fields survive decode, encode and text byte-exact") {
  const schema_registry reg = must(testing::make_test_registry());
  const type_id vec3 = type_id_of(tt::vec3);
  const value v = vec3_with_extra();
  const auto bytes = must(encode(v));
  const value decoded = must(decode(bytes, unknown_policy::preserve, vec3, reg));
  CHECK(decoded == v);
  CHECK(must(encode(decoded)) == bytes);
  const std::string text = must(to_text(decoded, vec3, reg));
  CHECK(text == R"({"x":1.0,"y":2.0,"z":3.0,"?9":"slb:0d02010706667574757265020429"})");
  const value parsed = must(from_text(text, vec3, reg));
  CHECK(must(encode(parsed)) == bytes);
  // The default policy is preserve, with or without a schema.
  CHECK(must(decode(bytes)) == v);
}

TEST_CASE("FAB-INV-SER-08: strict refuses an undeclared field with its offset") {
  const schema_registry reg = must(testing::make_test_registry());
  const type_id vec3 = type_id_of(tt::vec3);
  const auto bytes = must(encode(vec3_with_extra()));
  const auto r = decode(bytes, unknown_policy::strict, vec3, reg);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::decode_unknown_field);
  // 0x0D, count, then three fields of 1 + 9 bytes: field number 9 sits at offset 32.
  CHECK(r.error().message.starts_with("R-SER-UNKNOWN at offset 32:"));
  // Without a schema strict has nothing to judge.
  CHECK(decode(bytes, unknown_policy::strict).has_value());
  // A reserved field number is not declared either.
  const value entity = must(testing::sample_entity("hero"));
  const value with_reserved = must(entity.with({path_segment::field(7)}, value::uint(1)));
  const auto e =
      decode(must(encode(with_reserved)), unknown_policy::strict, type_id_of(tt::entity), reg);
  REQUIRE_FALSE(e.has_value());
  CHECK(e.error().code == errc::decode_unknown_field);
  CHECK(decode(must(encode(entity)), unknown_policy::strict, type_id_of(tt::entity), reg)
            .has_value());
}

TEST_CASE("FAB-INV-SER-08: strict follows the schema into containers and variant cases") {
  const schema_registry reg = must(testing::make_test_registry());
  const value entity = must(testing::sample_entity("hero"));
  // An unknown field inside the transform component of the components oset.
  const id_bytes element = testing::test_id(id_class::element, "hero/transform").bytes();
  const auto at = path{path_segment::field(4), path_segment::element(element),
                       path_segment::field(1), path_segment::field(12)};
  const value deep = must(entity.with(at, value::uint(5)));
  const auto bytes = must(encode(deep));
  CHECK(must(decode(bytes, unknown_policy::preserve, type_id_of(tt::entity), reg)) == deep);
  const auto r = decode(bytes, unknown_policy::strict, type_id_of(tt::entity), reg);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::decode_unknown_field);
  // An unknown variant case is kept and spelled "?<case>" in text; strict judges only fields.
  const value odd = must(value::variant(9, str("later")));
  const type_id component = type_id_of(tt::component);
  const auto odd_bytes = must(encode(odd));
  CHECK(must(decode(odd_bytes, unknown_policy::strict, component, reg)) == odd);
  const std::string text = must(to_text(odd, component, reg));
  CHECK(text == R"({"?9":"slb:07056c61746572"})");
  CHECK(must(from_text(text, component, reg)) == odd);
}
