// RT-INV-SCHED-07 (SL-ARCH-003 ARCH3-R126): the exported schedule (sl-runtime-schedule/1,
// schemas/runtime/schedule.schema.json) is byte-identical across runs and worker counts. The
// schedule of schemas/runtime/examples/schedule.json exports exactly as that file; an unbuilt
// scheduler exports no phase; a schedule over four phases with edges, structural and exclusive
// systems and resources exports the same bytes twice and on 1, 2, 4 and 8 workers, and the
// policy is part of the export.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

struct lantern_position {
  float x = 0.0F;
};
struct lantern_velocity {
  float v = 0.0F;
};
struct lantern_energy {
  float e = 0.0F;
};

struct empty_body {
  void operator()(ecs::system_context & /*ctx*/) const noexcept {}
};

// schemas/runtime/examples/schedule.json
constexpr std::string_view example = R"({
  "format": "sl-runtime-schedule/1",
  "policy": "parallel",
  "phases": [
    {
      "phase": "tick",
      "systems": [
        {
          "index": 0,
          "name": "lantern_drift",
          "access": {
            "component_reads": [
              "lantern_velocity"
            ],
            "component_writes": [
              "lantern_position"
            ],
            "resource_reads": [],
            "resource_writes": [],
            "structural": false,
            "exclusive": false
          },
          "after": []
        },
        {
          "index": 1,
          "name": "lantern_glow_sum",
          "access": {
            "component_reads": [
              "lantern_energy"
            ],
            "component_writes": [],
            "resource_reads": [],
            "resource_writes": [
              3
            ],
            "structural": false,
            "exclusive": false
          },
          "after": []
        }
      ]
    }
  ]
}
)";

constexpr std::string_view unbuilt = R"({
  "format": "sl-runtime-schedule/1",
  "policy": "parallel",
  "phases": []
}
)";

void add(ecs::system_scheduler &s, std::string_view name, rt::phase ph, ecs::access_set access,
         std::vector<std::string_view> after = {}) {
  auto id = s.add(ecs::system_desc{.name = name,
                                   .ph = ph,
                                   .access = std::move(access),
                                   .before = {},
                                   .after = std::move(after),
                                   .fn = empty_body{},
                                   .chunked = false,
                                   .queries = {}});
  REQUIRE(id.has_value());
}

ecs::access_set access(std::vector<ecs::component_id> reads, std::vector<ecs::component_id> writes,
                       const std::vector<std::uint32_t> &resource_writes = {}) {
  ecs::access_set a;
  a.components.reads = std::move(reads);
  a.components.writes = std::move(writes);
  for (const std::uint32_t k : resource_writes) {
    a.resource_writes.push_back(ecs::resource_key{k});
  }
  return a;
}

// The richer schedule over the world_gen components.
std::string export_rich(std::uint32_t workers, ecs::execution_policy policy) {
  auto made = rtt::sched_rig::make(workers);
  REQUIRE(made.has_value());
  rtt::sched_rig &r = **made;
  const auto &id = r.ids.ids;
  ecs::system_scheduler &s = *r.systems;
  add(s, "rich.move", rt::phase::pre_tick, access({id[1]}, {id[0]}));
  add(s, "rich.steer", rt::phase::pre_tick, access({id[6]}, {id[1]}));
  add(s, "rich.score", rt::phase::pre_tick, access({id[0], id[1]}, {}, {9, 2}));
  add(s, "rich.decay", rt::phase::tick, access({}, {id[7]}), {"rich.heal"});
  add(s, "rich.heal", rt::phase::tick, access({id[6]}, {id[7]}));
  ecs::access_set spawn = access({id[0]}, {});
  spawn.structural = true;
  add(s, "rich.spawn", rt::phase::post_tick, std::move(spawn));
  ecs::access_set audit;
  audit.exclusive = true;
  audit.resource_reads.push_back(ecs::resource_key{2});
  add(s, "rich.audit", rt::phase::observe, std::move(audit));
  s.set_policy(policy);
  REQUIRE(r.build().has_value());
  std::string first = s.export_json();
  // cppcheck-suppress knownConditionTrueFalse ; a second export is the subject under test
  CHECK(s.export_json() == first);
  return first;
}

} // namespace

TEST_CASE("RT-INV-SCHED-07: the example schedule exports exactly as the schema example") {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"sched.export.arena"};
  mem::tracking_resource world_memory{"sched.export.world"};
  jobs::job_config jc;
  jc.workers = 2;
  jc.capacity = 256;
  auto js = jobs::job_system::start(plat.get(), std::move(jc));
  REQUIRE(js.has_value());
  mem::frame_arena arena(upstream, 2, 65536);
  auto frames = rt::frame_scheduler::make(**js, plat.get(), arena);
  REQUIRE(frames.has_value());
  ecs::component_registry registry;
  auto pos = registry.add<lantern_position>("lantern_position");
  auto vel = registry.add<lantern_velocity>("lantern_velocity");
  auto energy = registry.add<lantern_energy>("lantern_energy");
  REQUIRE(pos.has_value());
  REQUIRE(vel.has_value());
  REQUIRE(energy.has_value());
  REQUIRE(registry.freeze().has_value());
  auto w = ecs::world::make(registry, world_memory);
  REQUIRE(w.has_value());
  ecs::command_queue queue(registry, arena);
  ecs::system_scheduler s(**js);
  CHECK(s.export_json() == unbuilt);
  add(s, "lantern_drift", rt::phase::tick, access({*vel}, {*pos}));
  add(s, "lantern_glow_sum", rt::phase::tick, access({*energy}, {}, {3}));
  CHECK(s.export_json() == unbuilt); // phases appear at build
  REQUIRE(s.build(**frames, **w, queue).has_value());
  CHECK(s.export_json() == example);
}

TEST_CASE("RT-INV-SCHED-07: the export is byte-identical across runs and worker counts") {
  const std::string reference = export_rich(1, ecs::execution_policy::parallel);
  for (const std::uint32_t workers : {2U, 4U, 8U, 1U}) {
    CHECK(export_rich(workers, ecs::execution_policy::parallel) == reference);
  }
  // rich.steer writes what rich.move reads; rich.score reads what both write.
  CHECK(reference.find(R"("name": "rich.score",)") != std::string::npos);
  CHECK(reference.find(R"("after": [
            0
          ])") != std::string::npos);
  CHECK(reference.find(R"("after": [
            1
          ])") != std::string::npos);
  CHECK(reference.find(R"("exclusive": true)") != std::string::npos);
  CHECK(reference.find(R"("structural": true)") != std::string::npos);
  CHECK(reference.find(R"("phase": "observe")") != std::string::npos);
  CHECK(reference.find(R"("phase": "apply_inputs")") == std::string::npos);
  const std::string serial = export_rich(4, ecs::execution_policy::serial_canonical);
  CHECK(serial.find(R"("policy": "serial_canonical")") != std::string::npos);
  CHECK(serial == export_rich(2, ecs::execution_policy::serial_canonical));
}
