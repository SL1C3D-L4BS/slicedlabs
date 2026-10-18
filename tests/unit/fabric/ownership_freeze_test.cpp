// FAB-INV-OWN-04 (SL-ARCH-001 ARCH1-R38): a claim after freeze is registry_frozen.

#include "sl/core/error.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

#include <doctest/doctest.h>

namespace {

using namespace sl::fabric;

domain_spec content_scene() {
  return domain_spec{.name = "content.scene",
                     .owner = subsystem::content,
                     .state_classes = {state_class::document},
                     .types = {}};
}

} // namespace

TEST_CASE("FAB-INV-OWN-04: a claim after freeze is registry_frozen and changes nothing") {
  domain_registry reg;
  REQUIRE(claim_fabric_domains(reg).has_value());
  CHECK_FALSE(reg.frozen());
  REQUIRE(reg.freeze().has_value());
  CHECK(reg.frozen());
  const auto r = reg.claim(content_scene());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::registry_frozen);
  CHECK(r.error().message.contains("content.scene"));
  CHECK(reg.find("content.scene") == nullptr);
  CHECK(reg.domains().size() == 6);
  REQUIRE(reg.rejected().size() == 1);
  CHECK(reg.rejected()[0].code == sl::errc::registry_frozen);
  CHECK(reg.f2() == f2_report{.numerator = 0, .denominator = 6, .conflicts = 0});
  // A second owner is refused as frozen too: nothing reaches the frozen table.
  CHECK(reg.claim(domain_spec{.name = "fabric.history",
                              .owner = subsystem::gov,
                              .state_classes = {state_class::document},
                              .types = {}})
            .error()
            .code == sl::errc::registry_frozen);
  CHECK(reg.find("fabric.history")->owner == subsystem::fabric);
}

TEST_CASE("FAB-INV-OWN-04: bind_types after freeze is registry_frozen; freeze is idempotent") {
  domain_registry reg;
  REQUIRE(reg.claim(content_scene()).has_value());
  REQUIRE(reg.freeze().has_value());
  REQUIRE(reg.freeze().has_value());
  schema_registry schemas;
  REQUIRE(schemas.freeze().has_value());
  CHECK(reg.bind_types(schemas).error().code == sl::errc::registry_frozen);
  CHECK(reg.frozen());
  CHECK(reg.find("content.scene") != nullptr);
}
