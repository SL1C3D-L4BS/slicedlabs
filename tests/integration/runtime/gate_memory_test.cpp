// RT-INV-GATE-03 (SL-ARCH-003 ARCH3-R051, R062, R167, ADR-0028; WO-099): the gate scene at
// gate_test_scale shuts down with zero leaks, and every steady frame (past warmup_frames + 1, not
// the two frames after the restore, not a runtime.mem report frame) takes zero upstream
// allocations (frame arena and world memory) and emits zero log events.

#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/tools/demo/scene.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>

namespace demo = sl::runtime::demo;

TEST_CASE("RT-INV-GATE-03: zero leaks, zero upstream allocations and zero log events per steady "
          "frame") {
  demo::demo_options o;
  o.entities = static_cast<std::uint32_t>(sl::runtime::testing::scaled(demo::gate_lanterns / 10));
  o.workers = 4;
  o.evidence_dir =
      (std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "gate-test" / "memory").string();
  o.variants = false;
  o.hash_every = 50;
  const demo::demo_summary s = demo::run_demo(o);
  INFO(s.message);
  CHECK(s.code == demo::exit_code::ok);
  CHECK(s.leaks == 0);
  CHECK(s.memory.leaks.empty());
  CHECK(s.steady_frames >= demo::gate_ticks - 20U);
  CHECK(s.steady_upstream == 0);
  CHECK(s.steady_log_events == 0);
  CHECK(s.population > 0);
}
