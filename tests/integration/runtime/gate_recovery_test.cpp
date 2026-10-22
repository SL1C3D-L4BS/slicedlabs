// RT-INV-GATE-02 (SL-ARCH-003 ARCH3-R169, ARCH3-R029, ADR-0028; WO-099): in the gate scene at
// gate_test_scale, RFP5 (system.fail) armed for the frame after step 450 is recovered by restoring
// the step-300 snapshot; the re-run steps hash as in the first run and the final state hash equals
// a fault-free run's. Debug builds arm the real failpoint; builds without failpoints use the
// scripted fail of lantern.decay. Both runs hash every 50 steps.

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/tools/demo/scene.hpp"

#include "sl/runtime/faults.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace {

namespace demo = sl::runtime::demo;
namespace rt = sl::runtime;

[[nodiscard]] demo::demo_options scaled_options(const char *name) {
  demo::demo_options o;
  o.entities = static_cast<std::uint32_t>(rt::testing::scaled(demo::gate_lanterns / 10));
  o.workers = 4;
  o.evidence_dir = (std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "gate-test" / name).string();
  o.variants = false;
  o.hash_every = 50;
  return o;
}

} // namespace

TEST_CASE("RT-INV-GATE-02: the fault at 450 restores the step-300 snapshot and ends with the "
          "fault-free hash") {
  demo::demo_options faulted = scaled_options("recovery");
  bool armed_real = false;
  faulted.arm_fault = [&armed_real] {
    if constexpr (rt::faults_compiled) {
      armed_real = rt::testing::arm(rt::failpoint::rfp5, 1).has_value();
    }
    return armed_real;
  };
  faulted.disarm_fault = [] { rt::testing::disarm(rt::failpoint::rfp5); };
  const demo::demo_summary f = demo::run_demo(faulted);
  INFO(f.message);
  REQUIRE(f.code == demo::exit_code::ok);
  CHECK(armed_real == rt::faults_compiled);
  REQUIRE(f.fault.has_value());
  const rt::fault_report fr = f.fault.value_or(rt::fault_report{});
  CHECK(fr.kind == "system_fail");
  CHECK(fr.tick == demo::gate_fault_tick);
  CHECK(fr.action == "restore");
  REQUIRE(fr.restored_tick.has_value());
  CHECK(fr.restored_tick.value_or(0) == demo::gate_snapshot_every);
  CHECK_FALSE(fr.system.empty());
  CHECK(f.recoveries == 1);
  CHECK(f.hash_equal);
  const std::string fault_json = rt::to_json(fr);
  CHECK(fault_json.find(R"("tick": 450)") != std::string::npos);

  demo::demo_options clean = scaled_options("recovery-clean");
  clean.fault_tick.reset();
  const demo::demo_summary c = demo::run_demo(clean);
  INFO(c.message);
  REQUIRE(c.code == demo::exit_code::ok);
  CHECK_FALSE(c.fault.has_value());
  REQUIRE(f.fresh.size() == c.fresh.size());
  REQUIRE_FALSE(f.fresh.empty());
  CHECK(f.fresh == c.fresh);
  CHECK(f.fresh.back() == c.fresh.back());
}
