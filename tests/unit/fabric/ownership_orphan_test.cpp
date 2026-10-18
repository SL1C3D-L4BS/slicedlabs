// FAB-INV-OWN-02 (SL-ARCH-001 ARCH1-R37): a type with no domain is ownership_orphan.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace sl::fabric;

type_desc plain(std::string_view name, std::string_view domain) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = type_kind::struct_;
  t.domain = std::string(domain);
  field_desc f;
  f.no = 1;
  f.name = "value";
  f.type = type_id_of("f64");
  t.fields = {f};
  return t;
}

domain_registry claimed() {
  domain_registry reg;
  REQUIRE(reg.claim(domain_spec{.name = "content.scene",
                                .owner = subsystem::content,
                                .state_classes = {state_class::document},
                                .types = {}})
              .has_value());
  return reg;
}

bool mentions(const sl::result<void> &r, std::string_view text) {
  return !r && r.error().message.contains(text);
}

} // namespace

TEST_CASE("FAB-INV-OWN-02: a type with no domain, or an unclaimed one, is ownership_orphan") {
  domain_registry reg = claimed();
  schema_registry schemas;
  REQUIRE(schemas.add(plain("content.scene.Vec3", "content.scene")).has_value());
  REQUIRE(schemas.add(plain("content.scene.Loose", "")).has_value());
  REQUIRE(schemas.add(plain("sim.physics.Body", "sim.physics")).has_value());
  REQUIRE(schemas.freeze().has_value());
  const auto r = reg.bind_types(schemas);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::ownership_orphan);
  CHECK(mentions(r, "type 'content.scene.Loose' has no domain"));
  CHECK(mentions(r, "type 'sim.physics.Body' has domain 'sim.physics', which no subsystem claims"));
  // All or nothing: the well-placed type was not bound either.
  CHECK(reg.domain_of(type_id_of("content.scene.Vec3")) == nullptr);
}

TEST_CASE("FAB-INV-OWN-02: a type binds to its domain or the innermost claimed ancestor") {
  domain_registry reg = claimed();
  REQUIRE(reg.claim(domain_spec{.name = "content.scene.ui",
                                .owner = subsystem::content,
                                .state_classes = {state_class::document},
                                .types = {}})
              .has_value());
  schema_registry schemas;
  REQUIRE(schemas.add(plain("content.scene.Vec3", "content.scene")).has_value());
  REQUIRE(schemas.add(plain("content.scene.Deep", "content.scene.lights.spot")).has_value());
  REQUIRE(schemas.add(plain("content.scene.Hud", "content.scene.ui")).has_value());
  REQUIRE(schemas.freeze().has_value());
  REQUIRE(reg.bind_types(schemas).has_value());
  CHECK(reg.domain_of(type_id_of("content.scene.Vec3"))->name == "content.scene");
  CHECK(reg.domain_of(type_id_of("content.scene.Deep"))->name == "content.scene");
  CHECK(reg.domain_of(type_id_of("content.scene.Hud"))->name == "content.scene.ui");
  // Built-in types belong to subsystem 02 and are not bound to a claimed domain.
  CHECK(reg.domain_of(type_id_of("f64")) == nullptr);
  CHECK(reg.owner_of("content.scene.lights")->name == "content.scene");
  CHECK(reg.owner_of("content.mesh") == nullptr);
}

TEST_CASE("FAB-INV-OWN-02: a domain outside the reserved prefixes is ownership_orphan") {
  domain_registry reg;
  for (const std::string_view bad :
       {"ui.hud", "editor.layout", "content", "", "Content.scene", "content..x", "fabric.event"}) {
    CAPTURE(bad);
    // fabric.event is under the fabric prefix, claimed by its owner, but not a fabric domain.
    const reserved_prefix *row = find_reserved_prefix(bad);
    const auto r = reg.claim(domain_spec{.name = std::string(bad),
                                         .owner = row != nullptr ? row->owner : subsystem::content,
                                         .state_classes = {state_class::document},
                                         .types = {}});
    CHECK(r.error().code == sl::errc::ownership_orphan);
  }
  CHECK(reg.domains().empty());
  CHECK(reg.rejected().size() == 7);
  CHECK(reg.f2().conflicts == 0);
}
