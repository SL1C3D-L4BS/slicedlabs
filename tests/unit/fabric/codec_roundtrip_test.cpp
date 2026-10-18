// FAB-INV-SER-05 (SL-ARCH-001 ARCH1-R29): binary to text to binary is the identity over 2000 seeded
// random values of generated types; decode(encode(v)) == v over 2000 seeded values of any kind.

#include "sl/fabric/codec.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

/// encode, decode, to_text, from_text, encode: every step succeeds and the bytes come back.
void round_trip(const value &v, type_id type, const schema_registry &reg) {
  const auto bytes = must(encode(v));
  const value decoded = must(decode(bytes, unknown_policy::strict, type, reg));
  CHECK(decoded == v);
  const std::string text = must(to_text(decoded, type, reg));
  const value parsed = must(from_text(text, type, reg));
  CHECK(parsed == v);
  CHECK(must(encode(parsed)) == bytes);
  CHECK(must(to_text(parsed, type, reg)) == text);
}

} // namespace

TEST_CASE("FAB-INV-SER-05: binary, text, binary is the identity over 2000 generated values") {
  std::size_t values = 0;
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    CAPTURE(seed);
    schema_registry reg;
    testing::value_gen gen(seed);
    const type_id t = must(gen.add_type(reg));
    REQUIRE(reg.freeze().has_value());
    for (int i = 0; i < 4; ++i) {
      round_trip(must(gen.conforming(reg, t)), t, reg);
      ++values;
    }
  }
  CHECK(values == 2000);
}

TEST_CASE("FAB-INV-SER-05: the test schemas round-trip through text") {
  const schema_registry reg = must(testing::make_test_registry());
  round_trip(must(testing::sample_entity("hero")), type_id_of(testing::test_types::entity), reg);
  round_trip(must(testing::sample_scene("level", 5)), type_id_of(testing::test_types::scene), reg);
  round_trip(must(testing::make_vec3(0.5, -1.25, 3e20)), type_id_of(testing::test_types::vec3),
             reg);
  testing::value_gen gen(41);
  for (const auto name : {testing::test_types::entity, testing::test_types::scene,
                          testing::test_types::mesh, testing::test_types::cooked_mesh}) {
    CAPTURE(name);
    for (int i = 0; i < 25; ++i) {
      round_trip(must(gen.conforming(reg, type_id_of(name))), type_id_of(name), reg);
    }
  }
}

TEST_CASE("FAB-INV-SER-05: decode(encode(v)) is v for 2000 values of every kind") {
  testing::value_gen gen(2026);
  for (int i = 0; i < 2000; ++i) {
    CAPTURE(i);
    const value v = gen.any();
    const auto bytes = must(encode(v));
    const value back = must(decode(bytes));
    CHECK(back == v);
    CHECK(must(encode(back)) == bytes);
  }
}

TEST_CASE("FAB-INV-SER-05: text that is not the canonical spelling is refused") {
  const schema_registry reg = must(testing::make_test_registry());
  const type_id vec3 = type_id_of(testing::test_types::vec3);
  CHECK(from_text(R"({"x":1.0,"y":2.0,"z":3.0})", vec3, reg).has_value());
  for (const char *bad : {R"({"x":1.0, "y":2.0,"z":3.0})", R"({"y":2.0,"x":1.0,"z":3.0})",
                          R"({"x":1,"y":2.0,"z":3.0})", R"({"x":1.00,"y":2.0,"z":3.0})",
                          R"({"x":-0.0,"y":2.0,"z":3.0})", R"({"x":1e0,"y":2.0,"z":3.0})",
                          R"({"x":1.0,"y":2.0,"z":3.0,"x":1.0})", R"( {"x":1.0,"y":2.0,"z":3.0})",
                          R"({"x":1.0,"y":2.0,"z":3.0)", R"({"x":NaN,"y":2.0,"z":3.0})"}) {
    CAPTURE(bad);
    const auto r = from_text(bad, vec3, reg);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::text_syntax);
  }
  // A field the type does not name and a value of the wrong kind are type errors.
  CHECK(from_text(R"({"x":1.0,"y":2.0,"w":3.0})", vec3, reg).error().code ==
        sl::errc::type_mismatch);
  CHECK(to_text(value::uint(3), vec3, reg).error().code == sl::errc::type_mismatch);
  CHECK(to_text(value::uint(3), type_id_of("no.such.Type"), reg).error().code ==
        sl::errc::schema_unknown);
}
