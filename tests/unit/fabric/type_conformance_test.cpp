// FAB-INV-TY-02 (SL-ARCH-001 ARCH1-R12): check() accepts a value iff it conforms to its type
// (presence, integer width ranges, enumerators, cases, ref targets) and returns its diagnostics
// sorted by (path, rule).

#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tt = sl::fabric::testing::test_types;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value str(std::string_view s) { return must(value::string(s)); }

path at(std::initializer_list<path_segment> segs) { return segs; }

const schema_registry &registry() {
  static const schema_registry reg = must(testing::make_test_registry());
  return reg;
}

/// The diagnostics of checking `v` against the type named `type`; empty when accepted.
std::vector<diagnostic_lite> diagnose(const value &v, std::string_view type,
                                      const schema_registry &reg = registry()) {
  std::vector<diagnostic_lite> out;
  const auto r = reg.check(v, type_id_of(type), out);
  CHECK(r.has_value() == out.empty());
  if (!r) {
    CHECK(r.error().code == sl::errc::type_mismatch);
  }
  return out;
}

std::vector<std::string> rules(const std::vector<diagnostic_lite> &d) {
  std::vector<std::string> out;
  out.reserve(d.size());
  for (const auto &x : d) {
    out.push_back(x.rule);
  }
  return out;
}

using strings = std::vector<std::string>;

/// A registry with one struct "t.I" holding a field of each integer kind.
schema_registry integer_registry() {
  schema_registry reg;
  type_desc t;
  t.name = "t.I";
  t.id = type_id_of(t.name);
  t.kind = type_kind::struct_;
  std::uint32_t no = 1;
  for (const std::string_view k : {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"}) {
    field_desc f;
    f.no = no++;
    f.name = std::string(k);
    f.type = type_id_of(k);
    f.presence = presence::optional;
    t.fields.push_back(std::move(f));
  }
  REQUIRE(reg.add(std::move(t)).has_value());
  REQUIRE(reg.freeze().has_value());
  return reg;
}

// "rule path message" of the first diagnostic, or empty.
std::string first_diagnostic(const std::vector<diagnostic_lite> &d) {
  if (d.empty()) {
    return {};
  }
  std::string out = d.front().rule;
  out += ' ';
  out += to_string(d.front().path);
  out += ' ';
  out += d.front().message;
  return out;
}

} // namespace

TEST_CASE("FAB-INV-TY-02: conforming values are accepted") {
  CHECK(diagnose(must(testing::sample_entity("hero")), tt::entity).empty());
  CHECK(diagnose(must(testing::sample_scene("level", 5)), tt::scene).empty());
  CHECK(diagnose(must(testing::make_vec3(0, 1, 2)), tt::vec3).empty());
  // A tagged type checks as its base.
  CHECK(diagnose(must(value::f64(2.5)), tt::meters).empty());
  CHECK(rules(diagnose(value::uint(2), tt::meters)) == strings{"R-TY-KIND"});
}

TEST_CASE("FAB-INV-TY-02: generated values conform to generated and test types") {
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    CAPTURE(seed);
    schema_registry reg;
    testing::value_gen gen(seed);
    const type_id t = must(gen.add_type(reg));
    REQUIRE(reg.freeze().has_value());
    for (int i = 0; i < 4; ++i) {
      const value v = must(gen.conforming(reg, t));
      const auto d = diagnose(v, reg.find(t)->name, reg);
      // The message is built outside CHECK_MESSAGE's context lambda (gcc -fanalyzer, WO-064).
      const std::string why = first_diagnostic(d);
      CHECK_MESSAGE(d.empty(), why);
    }
  }
  testing::value_gen gen(7);
  for (const std::string_view name : {tt::entity, tt::scene, tt::mesh, tt::cooked_mesh}) {
    for (int i = 0; i < 50; ++i) {
      CAPTURE(name);
      const auto d = diagnose(must(gen.conforming(registry(), type_id_of(name))), name);
      // The message is built outside CHECK_MESSAGE's context lambda (gcc -fanalyzer, WO-064).
      const std::string why = first_diagnostic(d);
      CHECK_MESSAGE(d.empty(), why);
    }
  }
}

TEST_CASE("FAB-INV-TY-02: presence, kinds and declared fields") {
  const value entity = must(testing::sample_entity("hero"));
  // A required field absent.
  const value no_components = must(entity.without(at({path_segment::field(4)})));
  const auto d = diagnose(no_components, tt::entity);
  REQUIRE(d.size() == 1);
  CHECK(d[0].rule == "R-TY-REQUIRED");
  CHECK(d[0].path == at({path_segment::field(4)}));
  // An optional field absent is fine.
  CHECK(diagnose(must(entity.without(at({path_segment::field(5)}))), tt::entity).empty());
  // The wrong kind.
  CHECK(rules(diagnose(must(entity.with(at({path_segment::field(1)}), value::uint(1))),
                       tt::entity)) == strings{"R-TY-KIND"});
  // An undeclared field and a reserved one (7).
  CHECK(rules(diagnose(must(entity.with(at({path_segment::field(9)}), value::uint(1))),
                       tt::entity)) == strings{"R-TY-FIELD"});
  CHECK(rules(diagnose(must(entity.with(at({path_segment::field(7)}), value::uint(1))),
                       tt::entity)) == strings{"R-TY-RESERVED"});
  // A struct where a struct is needed, nothing else.
  CHECK(rules(diagnose(value::uint(3), tt::entity)) == strings{"R-TY-KIND"});
}

TEST_CASE("FAB-INV-TY-02: integer width ranges") {
  const schema_registry reg = integer_registry();
  const auto one = [&](std::uint32_t no, value v) {
    return rules(diagnose(must(value::struct_({field{.no = no, .v = std::move(v)}})), "t.I", reg));
  };
  struct row {
    std::uint32_t no;
    std::uint64_t max; // largest uint
    bool is_signed;    // the largest nint magnitude is `max` too
  };
  const std::vector<row> rows = {
      {.no = 1, .max = 127, .is_signed = true},
      {.no = 2, .max = 32767, .is_signed = true},
      {.no = 3, .max = 2147483647, .is_signed = true},
      {.no = 4, .max = 9223372036854775807ULL, .is_signed = true},
      {.no = 5, .max = 255, .is_signed = false},
      {.no = 6, .max = 65535, .is_signed = false},
      {.no = 7, .max = 4294967295ULL, .is_signed = false},
      {.no = 8, .max = std::numeric_limits<std::uint64_t>::max(), .is_signed = false},
  };
  for (const row &r : rows) {
    CAPTURE(r.no);
    CHECK(one(r.no, value::uint(0)).empty());
    CHECK(one(r.no, value::uint(r.max)).empty());
    if (r.max != std::numeric_limits<std::uint64_t>::max()) {
      CHECK(one(r.no, value::uint(r.max + 1)) == strings{"R-TY-RANGE"});
    }
    if (r.is_signed) {
      CHECK(one(r.no, value::nint(r.max)).empty()); // the minimum, -(max + 1)
      CHECK(one(r.no, value::nint(r.max + 1)) == strings{"R-TY-RANGE"});
    } else {
      CHECK(one(r.no, value::integer(-1)) == strings{"R-TY-RANGE"});
    }
    CHECK(one(r.no, must(value::f64(1.0))) == strings{"R-TY-KIND"});
  }
  CHECK(one(1, value::integer(-128)).empty());
  CHECK(one(1, value::integer(-129)) == strings{"R-TY-RANGE"});
  CHECK(one(4, value::integer(std::numeric_limits<std::int64_t>::min())).empty());
}

TEST_CASE("FAB-INV-TY-02: enumerators, cases, ids and ref targets") {
  const value entity = must(testing::sample_entity("hero"));
  const path layer = at({path_segment::field(2)});
  CHECK(diagnose(must(entity.with(layer, value::uint(2))), tt::entity).empty());
  CHECK(rules(diagnose(must(entity.with(layer, value::uint(3))), tt::entity)) ==
        strings{"R-TY-ENUM"});

  // Variant case 4 is not a Component case.
  const record_id tag_elem = testing::test_id(id_class::element, "hero/tag");
  const path tag = at({path_segment::field(4), path_segment::element(tag_elem)});
  const value bad_case = must(value::variant(4, value::uint(1)));
  const auto d = diagnose(must(entity.with(tag, bad_case)), tt::entity);
  REQUIRE(d.size() == 1);
  CHECK(d[0].rule == "R-TY-CASE");
  CHECK(d[0].path == tag);
  // The payload of a case is checked under the case number.
  const value bad_payload = must(value::variant(3, value::uint(1)));
  const auto d2 = diagnose(must(entity.with(tag, bad_payload)), tt::entity);
  REQUIRE(d2.size() == 1);
  CHECK(d2[0].rule == "R-TY-KIND");
  CHECK(d2[0].path ==
        at({path_segment::field(4), path_segment::element(tag_elem), path_segment::field(3)}));

  // Ref targets: the parent of an Entity is an entity id, never an asset id.
  const path parent = at({path_segment::field(3)});
  const value ent = value::id(testing::test_id(id_class::entity, "boss"));
  const value ast = value::id(testing::test_id(id_class::asset, "boss"));
  CHECK(diagnose(must(entity.with(parent, ent)), tt::entity).empty());
  CHECK(rules(diagnose(must(entity.with(parent, ast)), tt::entity)) == strings{"R-TY-REF-TARGET"});
  CHECK(rules(diagnose(must(entity.with(parent, value::id(record_id{}))), tt::entity)) ==
        strings{"R-TY-ID"});
  id_bytes unassigned{};
  unassigned[0] = 200;
  unassigned[1] = 1;
  CHECK(rules(diagnose(must(entity.with(parent, value::id(unassigned))), tt::entity)) ==
        strings{"R-TY-ID"});
  // Refs inside an oset field use the field's targets (Scene.entities owned -> Entity).
  const value scene = must(testing::sample_scene("level", 2));
  const record_id slot = testing::test_id(id_class::element, "level/slot1");
  const path entry = at({path_segment::field(2), path_segment::element(slot)});
  CHECK(rules(diagnose(must(scene.with(entry, ast)), tt::scene)) == strings{"R-TY-REF-TARGET"});
}

TEST_CASE("FAB-INV-TY-02: diagnostics are complete and sorted by path then rule") {
  const value entity = must(testing::sample_entity("hero"));
  const record_id tag_elem = testing::test_id(id_class::element, "hero/tag");
  value v = must(entity.without(at({path_segment::field(1)})));         // R-TY-REQUIRED /1
  v = must(v.with(at({path_segment::field(2)}), value::uint(9)));       // R-TY-ENUM /2
  v = must(v.with(at({path_segment::field(9)}), value::boolean(true))); // R-TY-FIELD /9
  v = must(v.with(at({path_segment::field(4), path_segment::element(tag_elem)}),
                  must(value::variant(3, must(value::struct_({})))))); // R-TY-REQUIRED
  const path props_owner = at({path_segment::field(5), must(path_segment::key(str("owner")))});
  v = must(v.with(props_owner, value::uint(1))); // R-TY-KIND
  const value lods = must(value::list({must(value::f32(1.0F)), value::uint(1), value::uint(2)}));
  v = must(v.with(at({path_segment::field(6)}), lods)); // 2 x R-TY-KIND /6

  std::vector<diagnostic_lite> out;
  const auto r = registry().check(v, type_id_of(tt::entity), out);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::type_mismatch);
  CHECK(std::ranges::is_sorted(out));
  std::vector<std::string> got;
  got.reserve(out.size());
  for (const auto &d : out) {
    got.push_back(to_string(d.path) + " " + d.rule);
  }
  const std::string tag_path = "/4/<" + tag_elem.to_string() + ">/3/1";
  CHECK(got == strings{"/1 R-TY-REQUIRED", "/2 R-TY-ENUM", tag_path + " R-TY-REQUIRED",
                       "/5/[\"owner\"] R-TY-KIND", "/6 R-TY-KIND", "/6 R-TY-KIND",
                       "/9 R-TY-FIELD"});
  // List items keep the list's path and name their index in the message.
  CHECK(out[4].message.starts_with("list item 1:"));
  CHECK(out[5].message.starts_with("list item 2:"));
  // check() appends, and checking again gives the same diagnostics.
  std::vector<diagnostic_lite> again = {out.front()};
  CHECK_FALSE(registry().check(v, type_id_of(tt::entity), again).has_value());
  CHECK(again.size() == out.size() + 1);
  CHECK(std::equal(out.begin(), out.end(), again.begin() + 1));
}

TEST_CASE("FAB-INV-TY-02: check needs a frozen registry and a registered type") {
  schema_registry open;
  std::vector<diagnostic_lite> out;
  const auto unfrozen = open.check(value::uint(1), type_id_of("u8"), out);
  REQUIRE_FALSE(unfrozen.has_value());
  CHECK(unfrozen.error().code == sl::errc::schema_invalid);
  const auto unknown = registry().check(value::uint(1), type_id_of("t.Nope"), out);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == sl::errc::schema_unknown);
  CHECK(out.empty());
}
