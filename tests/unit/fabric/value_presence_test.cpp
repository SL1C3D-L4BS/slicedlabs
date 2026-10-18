// FAB-INV-TY-07 (SL-ARCH-001 ARCH1-R14): an absent field and a field present with its default
// value are distinct values; reading applies the default without storing it.

#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

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

const schema_registry &registry() {
  static const schema_registry reg = must(testing::make_test_registry());
  return reg;
}

const field_desc &field_of(std::string_view type, std::uint32_t no) {
  const type_desc *t = registry().find(type);
  REQUIRE(t != nullptr);
  const field_desc *f = t->find_field(no);
  REQUIRE(f != nullptr);
  return *f;
}

bool conforms(const value &v, std::string_view type) {
  std::vector<diagnostic_lite> out;
  return registry().check(v, type_id_of(type), out).has_value();
}

} // namespace

TEST_CASE("FAB-INV-TY-07: absent and present-equal-to-default are distinct values") {
  const value entity = must(testing::sample_entity("hero"));
  const field_desc &layer = field_of(tt::entity, 2);
  REQUIRE(layer.default_value.has_value());
  CHECK(layer.default_value == value::uint(0));

  const value explicit_default = must(entity.with({path_segment::field(2)}, value::uint(0)));
  CHECK_FALSE(entity.has_field(2));
  CHECK(explicit_default.has_field(2));
  CHECK_FALSE(explicit_default == entity);
  CHECK(explicit_default.fields().size() == entity.fields().size() + 1);
  // Both conform: presence is the writer's choice and is kept as written.
  CHECK(conforms(entity, tt::entity));
  CHECK(conforms(explicit_default, tt::entity));
  // Removing the field makes it absent again, not "default".
  const value removed = must(explicit_default.without({path_segment::field(2)}));
  CHECK(removed == entity);
  CHECK_FALSE(removed.has_field(2));
  // Editing another field keeps the explicit default present.
  const value edited =
      must(explicit_default.with({path_segment::field(1)}, must(value::string("other"))));
  CHECK(edited.has_field(2));
  CHECK(*edited.find_field(2) == value::uint(0));
}

TEST_CASE("FAB-INV-TY-07: reading applies the default without storing it") {
  const value entity = must(testing::sample_entity("hero"));
  const void *node = entity.node_address();
  const std::size_t count = entity.fields().size();

  // Absent with a default: the default is read.
  const auto layer = read_field(entity, field_of(tt::entity, 2));
  REQUIRE(layer.has_value());
  CHECK(layer == value::uint(0));
  // Present: the stored value is read, never the default.
  const value ui = must(entity.with({path_segment::field(2)}, value::uint(1)));
  CHECK(read_field(ui, field_of(tt::entity, 2)) == value::uint(1));
  // Absent without a default: nothing.
  CHECK_FALSE(read_field(entity, field_of(tt::entity, 3)).has_value());
  // Required and present.
  CHECK(read_field(entity, field_of(tt::entity, 1)) == must(value::string("hero")));
  // The read changed nothing.
  CHECK(entity.node_address() == node);
  CHECK(entity.fields().size() == count);
  CHECK_FALSE(entity.has_field(2));

  // Nested defaults: Transform.scale (1.0) and MeshRenderer.cast_shadows (true).
  const value transform =
      must(value::struct_({field{.no = 1, .v = must(testing::make_vec3(0, 0, 0))}}));
  CHECK(conforms(transform, tt::transform));
  CHECK(read_field(transform, field_of(tt::transform, 2)) == must(value::f64(1.0)));
  CHECK_FALSE(transform.has_field(2));
  const value renderer = must(
      value::struct_({field{.no = 1, .v = value::id(testing::test_id(id_class::asset, "rock"))}}));
  CHECK(conforms(renderer, tt::mesh_renderer));
  CHECK(read_field(renderer, field_of(tt::mesh_renderer, 2)) == value::boolean(true));
  CHECK(renderer.fields().size() == 1);
}

TEST_CASE("FAB-INV-TY-07: defaults belong to optional fields and conform to their type") {
  type_desc t;
  t.name = "t.D";
  t.id = type_id_of(t.name);
  t.kind = type_kind::struct_;
  field_desc f;
  f.no = 1;
  f.name = "n";
  f.type = type_id_of("u8");
  f.presence = presence::required;
  f.default_value = value::uint(3);
  t.fields = {f};
  schema_registry reg;
  const auto required = reg.add(t);
  REQUIRE_FALSE(required.has_value());
  CHECK(required.error().message.starts_with("R-TY-DEFAULT"));

  t.fields[0].presence = presence::optional;
  t.fields[0].default_value = value::uint(300); // outside u8
  REQUIRE(reg.add(t).has_value());
  const auto frozen = reg.freeze();
  REQUIRE_FALSE(frozen.has_value());
  CHECK(frozen.error().message.contains("R-TY-DEFAULT"));
  CHECK(frozen.error().message.contains("R-TY-RANGE"));
  CHECK_FALSE(reg.frozen());

  schema_registry ok;
  t.fields[0].default_value = value::uint(255);
  REQUIRE(ok.add(t).has_value());
  CHECK(ok.freeze().has_value());
}
