// RT-INV-BND-01 (SL-ARCH-003 ARCH3-R152, ADR-0027): document_source::load_plan lists every record
// of the projection grouped by document ascending, then by record id ascending, so ingest order is
// a pure function of the projection: equal document state gives equal plans whatever the commit
// order and batching that produced it, over 100 seeded projects (testing::fabric_host's seed).

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/runtime/bridge.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace fab = sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

rtt::host_spec project(std::uint64_t seed, bool large) {
  rtt::host_spec s;
  s.levels = large ? 3 : 2;
  s.bodies = large ? 12 : 3;
  s.markers = large ? 4 : 1;
  s.crates = large ? 2 : 1;
  s.domains = {"content"};
  s.seed = seed;
  return s;
}

struct planned {
  std::vector<link::load_entry> plan;
  fab::digest root;
  std::optional<fab::digest> revision;
};

planned plan_of(std::uint64_t seed) {
  const auto host = must(rtt::fabric_host::make(project(seed, false)));
  const link::document_source source = must(link::document_source::make(host->projection()));
  return planned{.plan = source.load_plan(),
                 .root = source.projection().root_digest(),
                 .revision = source.projection().revision_digest()};
}

// Ten seeds from `first`: equal roots and equal plans, all equal to seed 0's.
void check_seeds(std::uint64_t first) {
  const planned reference = plan_of(0);
  CHECK(reference.plan.size() == 7);
  for (std::uint64_t seed = first; seed < first + 10; ++seed) {
    const planned p = plan_of(seed);
    CHECK_MESSAGE(p.root == reference.root, "seed " << seed);
    CHECK_MESSAGE(p.plan == reference.plan, "seed " << seed);
  }
}

} // namespace

TEST_CASE("RT-INV-BND-01: load_plan groups the records by document ascending, then record id") {
  const auto host = must(rtt::fabric_host::make(project(7, true)));
  const fab::document_projection projection = host->projection();
  const link::document_source source = must(link::document_source::make(projection));
  const std::vector<link::load_entry> plan = source.load_plan();
  // Every record exactly once: 3 levels, 12 bodies, 4 markers, 2 crates.
  REQUIRE(plan.size() == projection.size());
  CHECK(plan.size() == 21);
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const link::load_entry &e = plan[i];
    CHECK(must(projection.document_of(e.record)) == e.document);
    CHECK(must(projection.get(e.record)).type == e.type);
    if (i > 0) {
      const link::load_entry &prev = plan[i - 1];
      const bool ordered =
          prev.document < e.document || (prev.document == e.document && prev.record < e.record);
      CHECK(ordered);
    }
  }
  // Records of no document (the null id) first, then each document as one run, in documents()
  // order.
  std::vector<fab::record_id> runs;
  for (const link::load_entry &e : plan) {
    if (!e.document.is_null() && (runs.empty() || runs.back() != e.document)) {
      runs.push_back(e.document);
    }
  }
  CHECK(runs ==
        std::vector<fab::record_id>(projection.documents().begin(), projection.documents().end()));
  // The plan is a value: asking twice gives the same list.
  CHECK(source.load_plan() == plan);
}

TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 1 to 10") { check_seeds(1); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 11 to 20") { check_seeds(11); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 21 to 30") { check_seeds(21); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 31 to 40") { check_seeds(31); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 41 to 50") { check_seeds(41); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 51 to 60") { check_seeds(51); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 61 to 70") { check_seeds(61); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 71 to 80") { check_seeds(71); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 81 to 90") { check_seeds(81); }
TEST_CASE("RT-INV-BND-01: equal projections give equal plans, seeds 91 to 100") {
  check_seeds(91);
  // The seeds did change the history: another commit order gives another revision.
  CHECK(plan_of(1).revision != plan_of(2).revision);
}
