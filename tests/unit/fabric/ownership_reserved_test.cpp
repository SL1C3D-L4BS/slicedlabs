// FAB-INV-OWN-03 (SL-ARCH-001 ARCH1-R36): the twelve reserved prefixes equal the subsystems of
// master plan section 2.2.

#include "sl/fabric/ownership.hpp"
#include "sl/fabric/type.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace sl::fabric;

struct expected_row {
  std::string_view prefix;
  std::string_view owner;
  std::vector<state_class> classes;
};

} // namespace

TEST_CASE("FAB-INV-OWN-03: one reserved prefix per subsystem 01..12, in owner order") {
  using sc = state_class;
  // master plan section 2.2, rows 01..12; ADR-0017 (runtime admits observations only).
  const std::array<expected_row, 12> want{{
      {.prefix = "shell", .owner = "01", .classes = {sc::document}},
      {.prefix = "fabric", .owner = "02", .classes = {sc::document, sc::derived}},
      {.prefix = "runtime", .owner = "03", .classes = {sc::observation}},
      {.prefix = "gfx", .owner = "04", .classes = {sc::derived}},
      {.prefix = "content", .owner = "05", .classes = {sc::document}},
      {.prefix = "sim", .owner = "06", .classes = {sc::document, sc::derived}},
      {.prefix = "code", .owner = "07", .classes = {sc::document, sc::derived}},
      {.prefix = "asset", .owner = "08", .classes = {sc::document, sc::derived}},
      {.prefix = "obs", .owner = "09", .classes = {sc::observation, sc::derived}},
      {.prefix = "collab", .owner = "10", .classes = {sc::document}},
      {.prefix = "verify", .owner = "11", .classes = {sc::observation, sc::document}},
      {.prefix = "gov", .owner = "12", .classes = {sc::document, sc::observation}},
  }};
  const auto rows = reserved_prefixes();
  REQUIRE(rows.size() == subsystem_count);
  std::size_t number = 0;
  for (const auto [row, w] : std::views::zip(rows, want)) {
    CAPTURE(w.prefix);
    ++number;
    CHECK(row.prefix == w.prefix);
    CHECK(owner_code(row.owner) == w.owner);
    CHECK(static_cast<std::size_t>(row.owner) == number);
    CHECK(std::vector<sc>(row.state_classes.begin(), row.state_classes.end()) == w.classes);
    CHECK_FALSE(row.owns.empty());
    CHECK(to_string(row.owner) == w.prefix);
    CHECK(subsystem_from_string(w.prefix) == row.owner);
    CHECK(subsystem_from_string(w.owner) == row.owner);
    CHECK(find_reserved_prefix(std::string(w.prefix) + ".x") == &row);
  }
  CHECK(owner_code(static_cast<subsystem>(0)) == "00");
  CHECK(owner_code(static_cast<subsystem>(13)) == "00");
  CHECK(to_string(static_cast<subsystem>(13)) == "unknown");
  CHECK_FALSE(subsystem_from_string("13").has_value());
  CHECK_FALSE(subsystem_from_string("ui").has_value());
  CHECK(find_reserved_prefix("ui.hud") == nullptr);
  // Game UI is content (content.ui), not shell (ADR-0017).
  CHECK(find_reserved_prefix("content.ui")->owner == subsystem::content);
}

TEST_CASE("FAB-INV-OWN-03: fabric domains, actor kinds and domain names") {
  const std::array<std::string_view, 6> fabric{"fabric.history",    "fabric.schema",
                                               "fabric.manifest",   "fabric.identity",
                                               "fabric.provenance", "fabric.edge"};
  CHECK(std::ranges::equal(fabric_domains(), fabric));
  const std::array<std::string_view, 5> kinds{"human", "tool", "plugin", "agent", "service"};
  CHECK(std::ranges::equal(actor_kind_names(), kinds));
  REQUIRE(default_deny_names().size() == 2);
  CHECK(default_deny_names()[0] == "agent");
  CHECK(default_deny_names()[1] == "service");

  CHECK(is_domain_name("content.scene"));
  CHECK(is_domain_name("sim.nav_mesh2.tiles"));
  CHECK_FALSE(is_domain_name("content"));
  CHECK_FALSE(is_domain_name("content."));
  CHECK_FALSE(is_domain_name(".scene"));
  CHECK_FALSE(is_domain_name("content.Scene"));
  CHECK_FALSE(is_domain_name("content.2d"));
  CHECK(domain_within("content.scene", "content"));
  CHECK(domain_within("content.scene", "content.scene"));
  CHECK(domain_within("content.scene.ui", "content.scene"));
  CHECK_FALSE(domain_within("content.scenery", "content.scene"));
  CHECK_FALSE(domain_within("content", "content.scene"));
  CHECK_FALSE(domain_within("content", ""));
}
