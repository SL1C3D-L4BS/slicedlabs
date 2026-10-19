// FAB-INV-OWN-09 (SL-ARCH-001 ARCH1-R41; master plan, Release gates, gate 1): after the
// conformance workload (conformance_suite.hpp: every registered type's fixtures written, edited,
// undone, redone and deleted through transactions), the live records without an owner, a stable
// id or a revision, over all live records, is 0; printed as numerator/denominator. The test also
// writes the scorecard input (build/fabric/scorecard-input.json, or SL_FABRIC_SCORECARD_INPUT)
// that tools/fabric/scorecard.py reads: F2, the F5 ledger delta, gate 1, commits, objects and
// validator invocations. The measure itself counts records whose type has no owner.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/conformance_registry.hpp"
#include "../../../engine/fabric/testing/conformance_suite.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <print>
#include <string>
#include <utility>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

} // namespace

TEST_CASE("FAB-INV-OWN-09: records without owner, stable id or revision over all records is 0 "
          "after the conformance workload") {
  const tg::conformance_run run =
      must(tg::run_conformance(must(tg::builtin_fixture_registry(SL_FABRIC_FIXTURES_DIR))));
  std::println("gate-1: records without owner, stable id or revision {}/{}", run.gate1.numerator,
               run.gate1.denominator);
  for (const std::string &o : run.gate1.offenders) {
    MESSAGE(o);
  }
  CHECK(run.gate1.denominator > 0);
  CHECK(run.gate1.numerator == 0);
  CHECK(run.f2.numerator == 0);
  CHECK(run.f2.denominator > 0);
  if (f5::enabled) {
    CHECK(run.counters.ledger.mutations > 0);
    CHECK(run.counters.ledger.bypassing == 0);
  }
  CHECK(run.counters.validation_passes >= run.counters.commits);

  const std::string path = tg::scorecard_input_path(SL_FABRIC_FIXTURES_DIR);
  const auto written = tg::write_text_file(path, tg::scorecard_input_json(run));
  REQUIRE_MESSAGE(written.has_value(), (written ? std::string() : written.error().message));
  std::println("gate-1: scorecard input {}", path);
}

TEST_CASE("FAB-INV-OWN-09: a record whose type has no owner counts in the numerator") {
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_memory(must(tg::scene_options(clock, 9))));
  (void)must(tg::commit_ops(ws->manager(), tg::tx_origin_of(tg::tx_editor()),
                            {must(tg::create_entity("lamp")), must(tg::create_entity("rope"))}));
  const snapshot head = must(ws->manager().head());
  const tg::gate1_report owned = must(tg::measure_gate1(head, ws->schemas(), ws->domains()));
  CHECK(owned.numerator == 0);
  CHECK(owned.denominator == 2);
  const domain_registry nobody;
  const tg::gate1_report orphaned = must(tg::measure_gate1(head, ws->schemas(), nobody));
  CHECK(orphaned.numerator == 2);
  CHECK(orphaned.denominator == 2);
  REQUIRE(orphaned.offenders.size() == 2);
  CHECK(orphaned.offenders[0].find("no owner") != std::string::npos);
}
