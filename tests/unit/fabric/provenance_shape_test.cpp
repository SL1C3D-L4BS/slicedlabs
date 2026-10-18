// FAB-INV-PRV-02 (SL-ARCH-001 ARCH1-R43): the provenance record has exactly the five F3 fields and
// none of the build-record fields schema, target, artifact, built_at.

#include "sl/core/error.hpp"
#include "sl/fabric/gen/fabric.provenance.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

const type_desc &provenance_type(const schema_registry &reg) {
  const type_desc *t = reg.find(type_id_of("fabric.provenance.provenance"));
  REQUIRE(t != nullptr);
  return *t;
}

schema_registry registry() {
  schema_registry reg;
  REQUIRE(gen::fabric_provenance::register_types(reg).has_value());
  return reg;
}

std::vector<std::string_view> build_record_fields() {
  return {"schema", "target", "artifact", "built_at"};
}

std::vector<std::string_view> f3_names() { return {f3_field_names.begin(), f3_field_names.end()}; }

} // namespace

TEST_CASE("FAB-INV-PRV-02: the descriptor has the five F3 fields in order and nothing else") {
  const schema_registry reg = registry();
  const type_desc &t = provenance_type(reg);
  const std::vector<std::string_view> names = f3_names();
  REQUIRE(t.fields.size() == names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    CAPTURE(names[i]);
    CHECK(t.fields[i].no == i + 1);
    CHECK(t.fields[i].name == names[i]);
    // Optional on the wire: a missing field is provenance_incomplete, not a decode error.
    CHECK(t.fields[i].presence == presence::optional);
  }
}

TEST_CASE("FAB-INV-PRV-02: none of the build-record fields is present") {
  const schema_registry reg = registry();
  const type_desc &t = provenance_type(reg);
  for (const std::string_view banned : build_record_fields()) {
    CAPTURE(banned);
    CHECK(std::ranges::none_of(t.fields, [&](const field_desc &f) { return f.name == banned; }));
    CHECK(std::ranges::none_of(f3_field_names,
                               [&](std::string_view name) { return name == banned; }));
  }
}

TEST_CASE("FAB-INV-PRV-02: a full record encodes exactly the field numbers 1 to 5") {
  provenance p;
  p.input_ids = std::vector<record_id>{well_known(id_class::asset, 1)};
  p.input_revisions = std::vector<digest>{digest{}};
  p.toolchain_id = "tool@1";
  p.parameters_digest = digest{};
  p.environment_hash = digest{};
  const value v = must(p.to_value());
  REQUIRE(v.kind() == value_kind::struct_);
  REQUIRE(v.fields().size() == 5);
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(v.fields()[i].no == i + 1);
  }
  // A record carrying a build-record field is refused by the strict typed decoder.
  const value extra = must(v.with(path{path_segment::field(6)}, must(value::string("target"))));
  CHECK_FALSE(provenance::from_value(extra).has_value());
}

TEST_CASE("FAB-INV-PRV-02: the generated type is the one provenance.hpp exports") {
  static_assert(std::is_same_v<provenance, gen::fabric_provenance::provenance>);
  static_assert(std::is_same_v<epistemic_class, gen::fabric_provenance::epistemic_class>);
  CHECK(static_cast<std::uint32_t>(epistemic_class::observed) == 1);
  CHECK(static_cast<std::uint32_t>(epistemic_class::accepted) == 6);
  CHECK(epistemic_classes.size() == 6);
}
