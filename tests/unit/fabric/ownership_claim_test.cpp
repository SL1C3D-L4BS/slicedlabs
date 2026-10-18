// FAB-INV-OWN-01 (SL-ARCH-001 ARCH1-R36): a second claim with another owner is
// ownership_conflict naming both; F2 numerator stays 0.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/type.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

domain_spec spec(std::string_view name, subsystem owner,
                 std::vector<state_class> classes = {state_class::document},
                 std::vector<type_id> types = {}) {
  return domain_spec{.name = std::string(name),
                     .owner = owner,
                     .state_classes = std::move(classes),
                     .types = std::move(types)};
}

bool mentions(const sl::result<void> &r, std::string_view text) {
  return !r && r.error().message.contains(text);
}

} // namespace

TEST_CASE("FAB-INV-OWN-01: a second claim by another owner is ownership_conflict naming both") {
  domain_registry reg;
  REQUIRE(reg.claim(spec("content.scene", subsystem::content)).has_value());
  const auto r = reg.claim(spec("content.scene", subsystem::sim));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::ownership_conflict);
  CHECK(mentions(r, "05 (content)"));
  CHECK(mentions(r, "06 (sim)"));
  // The claim is refused, recorded and counted; the domain keeps its one owner.
  REQUIRE(reg.rejected().size() == 1);
  CHECK(reg.rejected()[0].domain == "content.scene");
  CHECK(reg.rejected()[0].claimant == subsystem::sim);
  CHECK(reg.rejected()[0].holder == subsystem::content);
  CHECK(reg.rejected()[0].code == sl::errc::ownership_conflict);
  REQUIRE(reg.find("content.scene") != nullptr);
  CHECK(reg.find("content.scene")->owner == subsystem::content);
  CHECK(reg.f2() == f2_report{.numerator = 0, .denominator = 1, .conflicts = 1});
}

TEST_CASE("FAB-INV-OWN-01: a claim under another subsystem's prefix is ownership_conflict") {
  domain_registry reg;
  const auto r = reg.claim(spec("content.physics", subsystem::sim));
  CHECK(r.error().code == sl::errc::ownership_conflict);
  CHECK(mentions(r, "reserved to 05 (content)"));
  CHECK(mentions(r, "06 (sim)"));
  REQUIRE(reg.rejected().size() == 1);
  CHECK(reg.rejected()[0].holder == subsystem::content);
  CHECK(reg.domains().empty());
  CHECK(reg.f2() == f2_report{.numerator = 0, .denominator = 0, .conflicts = 1});
}

TEST_CASE("FAB-INV-OWN-01: the fabric domains are owned by 02 and cannot be claimed again") {
  domain_registry reg;
  REQUIRE(claim_fabric_domains(reg).has_value());
  CHECK(reg.domains().size() == fabric_domains().size());
  for (const std::string_view d : fabric_domains()) {
    CAPTURE(d);
    REQUIRE(reg.find(d) != nullptr);
    CHECK(reg.find(d)->owner == subsystem::fabric);
  }
  for (const subsystem other : {subsystem::content, subsystem::gov, subsystem::runtime}) {
    const auto r = reg.claim(spec("fabric.history", other));
    CHECK(r.error().code == sl::errc::ownership_conflict);
    CHECK(mentions(r, "02 (fabric)"));
    CHECK(mentions(r, owner_code(other)));
  }
  const f2_report f2 = reg.f2();
  CHECK(f2.numerator == 0);
  CHECK(f2.denominator == 6);
  CHECK(f2.conflicts == 3);
  CHECK(reg.freeze().has_value());
}

TEST_CASE("FAB-INV-OWN-01: the same owner twice, state classes and types") {
  domain_registry reg;
  const type_id body = type_id_of("sim.physics.Body");
  REQUIRE(
      reg.claim(spec("sim.physics", subsystem::sim, {state_class::document}, {body})).has_value());
  CHECK(reg.domain_of(body) == reg.find("sim.physics"));
  // A repeated claim by the owner is no second owner: already_exists, not a conflict.
  CHECK(reg.claim(spec("sim.physics", subsystem::sim)).error().code == sl::errc::already_exists);
  // A type belongs to one domain.
  const auto t = reg.claim(spec("sim.nav", subsystem::sim, {state_class::document}, {body}));
  CHECK(t.error().code == sl::errc::ownership_conflict);
  CHECK(mentions(t, "sim.physics"));
  // R-OWN-STATE: runtime admits observation records only (ADR-0017).
  const auto s = reg.claim(spec("runtime.live", subsystem::runtime, {state_class::document}));
  CHECK(s.error().code == sl::errc::schema_invalid);
  CHECK(mentions(s, "R-OWN-STATE"));
  CHECK(reg.claim(spec("runtime.capture", subsystem::runtime, {state_class::observation}))
            .has_value());
  CHECK(reg.claim(spec("gov.audit", subsystem::gov, {})).error().code == sl::errc::schema_invalid);
  CHECK(reg.f2() == f2_report{.numerator = 0, .denominator = 2, .conflicts = 1});
  CHECK(reg.rejected().size() == 4);
}
