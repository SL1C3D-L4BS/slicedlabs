// RT-INV-GATE-04 (SL-ARCH-003 ARCH3-R169, ARCH3-R148 to R161, SL-GOV-006 F5, ADR-0028; WO-099):
// in the gate scene at gate_test_scale the runtime's observations reach Fabric only as host
// commits through the proposal sink: every proposed patch is one host commit, none is rejected,
// the F5 ledger counts one store mutation per workspace commit (the seed commits, the observation
// commits and the setup commits of an empty run) and none bypassing the transaction manager. Writes
// SL_RUNTIME_EVIDENCE_ROOT/gate/f5.json (sl-runtime-f5/1, gate_evidence); f5.hpp is readable here
// and nowhere under engine/runtime.

#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/tools/demo/scene.hpp"

#include "sl/fabric/f5.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>

namespace {

namespace demo = sl::runtime::demo;
namespace f5 = sl::fabric::f5;

[[nodiscard]] demo::demo_options options(std::uint32_t ticks) {
  demo::demo_options o;
  o.ticks = ticks;
  o.entities = static_cast<std::uint32_t>(sl::runtime::testing::scaled(demo::gate_lanterns / 10));
  o.workers = 4;
  o.evidence_dir =
      (std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "gate-test" / "fabric").string();
  o.variants = false;
  o.hash_every = 50;
  return o;
}

} // namespace

TEST_CASE("RT-INV-GATE-04: observations are host commits through the sink and F5 bypassing is 0") {
  // One step: the setup commits of a stack (domain claim, types, seed) and no observation.
  const f5::ledger before_setup = f5::read();
  const demo::demo_summary setup = demo::run_demo(options(1));
  const f5::ledger after_setup = f5::read();
  REQUIRE(setup.code == demo::exit_code::ok);
  CHECK(setup.commits == 0);
  const std::uint64_t setup_mutations = after_setup.mutations - before_setup.mutations;

  const f5::ledger before = f5::read();
  const demo::demo_summary s = demo::run_demo(options(demo::gate_ticks));
  const f5::ledger after = f5::read();
  INFO(s.message);
  REQUIRE(s.code == demo::exit_code::ok);
  CHECK(s.records == 256);
  CHECK(s.commits > 0);
  CHECK(s.commits == s.proposed_patches);
  CHECK(s.proposed_changes > 0);
  CHECK(s.rejected == 0);
  CHECK(s.watched_changes > 0);
  const std::uint64_t mutations = after.mutations - before.mutations;
  const std::uint64_t bypassing = after.bypassing - before.bypassing;
  CHECK(bypassing == 0);
  CHECK(after.bypassing == 0);
  if constexpr (f5::enabled) {
    CHECK(setup_mutations > 0);
    CHECK(mutations == setup_mutations + s.commits);
  }

  const std::filesystem::path dir = std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "gate";
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / "f5.json", std::ios::binary | std::ios::trunc);
  out << std::format("{{\n  \"format\": \"sl-runtime-f5/1\",\n  \"mutations\": {},\n"
                     "  \"bypassing\": {},\n  \"host_commits\": {},\n  \"enabled\": {}\n}}\n",
                     mutations, bypassing, s.commits, f5::enabled);
  CHECK(out.good());
}
