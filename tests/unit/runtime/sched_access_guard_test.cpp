// RT-INV-SCHED-05 (SL-ARCH-003 ARCH3-R125): an undeclared write fails the system with
// undeclared_access. With the guard compiled in (NDEBUG undefined: Debug and sanitizer builds), a
// system that declares a read of gen.velocity and iterates a write query over it visits nothing
// and fails with undeclared_access; so do a write through system_context::resource of a resource
// declared read, par_for_each over an undeclared component and commands() in a system that is
// neither structural nor exclusive (its commands are discarded at the phase boundary). Declared
// access and exclusive systems (which run on the owner thread) pass. add refuses, in every build,
// a desc whose queries need access its access_set lacks (access_conflict). RFP5 fails a system
// after its body (WO-109: the failpoint table's site).

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;
using sl::errc;

using velocity_writes = ecs::query<ecs::write<rtt::gen_velocity>>;

constexpr ecs::resource_key counter_key{7};

struct counter {
  std::uint64_t value = 0;
};

struct probe {
  std::atomic<std::uint32_t> visited{0};
  std::atomic<std::uint32_t> runs{0};
  std::atomic<bool> stopped{false};
  std::atomic<bool> on_owner{false};
  std::atomic<errc> par_error{errc::ok};
  std::thread::id owner = std::this_thread::get_id();
};

enum class action : std::uint8_t {
  write_query,
  par_write_query,
  resource_write,
  resource_read,
  commands,
};

struct guarded_body {
  probe *p = nullptr;
  action act = action::write_query;
  velocity_writes q;

  void operator()(ecs::system_context &ctx) noexcept {
    p->runs.fetch_add(1);
    p->on_owner.store(std::this_thread::get_id() == p->owner);
    switch (act) {
    case action::write_query:
      ctx.for_each(q, [this](ecs::entity /*e*/, rtt::gen_velocity &v) noexcept {
        v.dx += 1.0;
        p->visited.fetch_add(1);
      });
      break;
    case action::par_write_query: {
      const auto ran = ctx.par_for_each(
          q, [this](std::uint32_t /*batch*/, ecs::entity /*e*/, rtt::gen_velocity &v) noexcept {
            v.dx += 1.0;
            p->visited.fetch_add(1);
          });
      p->par_error.store(ran ? errc::ok : ran.error().code);
      break;
    }
    case action::resource_write: {
      auto c = ctx.resource<counter>(counter_key);
      if (c) {
        ++(*c)->value;
        p->visited.fetch_add(1);
      }
      break;
    }
    case action::resource_read: {
      auto c = ctx.resource<const counter>(counter_key);
      if (c) {
        p->visited.fetch_add(static_cast<std::uint32_t>((*c)->value + 1));
      }
      break;
    }
    case action::commands:
      ctx.commands().destroy(ecs::entity{});
      p->visited.fetch_add(1);
      break;
    }
    p->stopped.store(ctx.stopped());
  }
};

struct rig_with_rows {
  std::unique_ptr<rtt::sched_rig> r;

  rig_with_rows() {
    auto made = rtt::sched_rig::make(2);
    REQUIRE(made.has_value());
    r = std::move(*made);
    for (int i = 0; i < 10; ++i) {
      REQUIRE(r->world->create_with(rtt::gen_velocity{.dx = 0.0, .dy = 0.0}).has_value());
    }
    REQUIRE(r->world->add_resource(counter_key, counter{}).has_value());
  }

  std::uint32_t add(probe &p, action act, ecs::access_set access, bool chunked = false) const {
    auto q = velocity_writes::make(r->registry);
    REQUIRE(q.has_value());
    auto id = r->systems->add(
        ecs::system_desc{.name = "guarded",
                         .ph = rt::phase::tick,
                         .access = std::move(access),
                         .before = {},
                         .after = {},
                         .fn = guarded_body{.p = &p, .act = act, .q = std::move(*q)},
                         .chunked = chunked,
                         .queries = {}});
    REQUIRE(id.has_value());
    return id.value_or(0);
  }

  // Runs frames until one ran a tick.
  void tick() const {
    for (int f = 0; f < 4; ++f) {
      auto s = r->step();
      REQUIRE(s.has_value());
      if (s->ticks_run > 0) {
        return;
      }
    }
    FAIL("no tick ran");
  }

  [[nodiscard]] ecs::component_id velocity() const { return r->ids.ids.at(1); }
};

ecs::access_set reads_velocity(const rig_with_rows &g) {
  ecs::access_set a;
  a.components.reads.push_back(g.velocity());
  return a;
}

ecs::access_set writes_velocity(const rig_with_rows &g) {
  ecs::access_set a;
  a.components.writes.push_back(g.velocity());
  return a;
}

// Checks the outcome of one guarded run: undeclared_access with the guard, a clean run without.
void check_violation(const rig_with_rows &g, std::uint32_t id, const probe &p) {
  CHECK(p.runs.load() == 1);
  if constexpr (ecs::access_guard_enabled) {
    CHECK(g.r->systems->last_error(id) == errc::undeclared_access);
    CHECK(g.r->systems->failures() == 1);
    CHECK(p.stopped.load());
  } else {
    CHECK(g.r->systems->last_error(id) == errc::ok);
    CHECK(g.r->systems->failures() == 0);
  }
}

} // namespace

TEST_CASE("RT-INV-SCHED-05: an undeclared write query fails the system with undeclared_access") {
  rig_with_rows g;
  probe p;
  const std::uint32_t id = g.add(p, action::write_query, reads_velocity(g));
  REQUIRE(g.r->build().has_value());
  g.tick();
  check_violation(g, id, p);
  CHECK(p.visited.load() == (ecs::access_guard_enabled ? 0U : 10U));
}

TEST_CASE("RT-INV-SCHED-05: an undeclared par_for_each write fails the system") {
  for (const bool chunked : {false, true}) {
    rig_with_rows g;
    probe p;
    const std::uint32_t id = g.add(p, action::par_write_query, reads_velocity(g), chunked);
    REQUIRE(g.r->build().has_value());
    g.tick();
    check_violation(g, id, p);
    CHECK(p.visited.load() == (ecs::access_guard_enabled ? 0U : 10U));
    CHECK(p.par_error.load() == (ecs::access_guard_enabled ? errc::undeclared_access : errc::ok));
  }
}

TEST_CASE("RT-INV-SCHED-05: a write to a resource declared read fails the system") {
  rig_with_rows g;
  probe p;
  ecs::access_set a;
  a.resource_reads.push_back(counter_key);
  const std::uint32_t id = g.add(p, action::resource_write, std::move(a));
  REQUIRE(g.r->build().has_value());
  g.tick();
  check_violation(g, id, p);
  auto c = g.r->world->resource<counter>(counter_key);
  REQUIRE(c.has_value());
  CHECK((*c)->value == (ecs::access_guard_enabled ? 0U : 1U));
}

TEST_CASE("RT-INV-SCHED-05: commands in a system that is not structural fail it") {
  rig_with_rows g;
  probe p;
  const std::uint32_t id = g.add(p, action::commands, ecs::access_set{});
  REQUIRE(g.r->build().has_value());
  g.tick();
  check_violation(g, id, p);
  if constexpr (ecs::access_guard_enabled) {
    REQUIRE_FALSE(g.r->systems->last_flush().has_value());
    CHECK(g.r->systems->last_flush().error().code == errc::job_failed); // discarded
  }
}

TEST_CASE("RT-INV-SCHED-05: declared access passes and exclusive systems run on the owner") {
  {
    rig_with_rows g;
    probe p;
    const std::uint32_t id = g.add(p, action::write_query, writes_velocity(g));
    REQUIRE(g.r->build().has_value());
    g.tick();
    CHECK(g.r->systems->last_error(id) == errc::ok);
    CHECK(p.visited.load() == 10);
    CHECK_FALSE(p.stopped.load());
  }
  {
    rig_with_rows g;
    probe p;
    ecs::access_set a;
    a.resource_reads.push_back(counter_key);
    const std::uint32_t id = g.add(p, action::resource_read, std::move(a));
    REQUIRE(g.r->build().has_value());
    g.tick();
    CHECK(g.r->systems->last_error(id) == errc::ok);
    CHECK(p.visited.load() == 1);
  }
  {
    rig_with_rows g;
    probe p;
    ecs::access_set a;
    a.exclusive = true;
    const std::uint32_t id = g.add(p, action::commands, std::move(a));
    REQUIRE(g.r->build().has_value());
    g.tick();
    CHECK(g.r->systems->last_error(id) == errc::ok);
    CHECK(p.on_owner.load());
    CHECK(g.r->systems->last_flush().has_value());
  }
}

TEST_CASE("RT-INV-SCHED-05: add refuses queries that need more than the access_set declares") {
  rig_with_rows g;
  probe p;
  auto q = velocity_writes::make(g.r->registry);
  REQUIRE(q.has_value());
  const ecs::component_access needed = q->access();
  auto id = g.r->systems->add(
      ecs::system_desc{.name = "greedy",
                       .ph = rt::phase::tick,
                       .access = reads_velocity(g),
                       .before = {},
                       .after = {},
                       .fn = guarded_body{.p = &p, .act = action::write_query, .q = std::move(*q)},
                       .chunked = false,
                       .queries = {needed}});
  REQUIRE_FALSE(id.has_value());
  CHECK(id.error().code == errc::access_conflict);
}

TEST_CASE("RT-INV-SCHED-05: RFP5 fails a system after its body") {
  rig_with_rows g;
  probe p;
  const std::uint32_t id = g.add(p, action::write_query, writes_velocity(g));
  REQUIRE(g.r->build().has_value());
  auto armed = rtt::arm(rt::failpoint::rfp5, 1);
  if (!armed) {
    CHECK(armed.error().code == errc::unsupported); // failpoints compiled out
    return;
  }
  g.tick();
  rtt::disarm(rt::failpoint::rfp5);
  CHECK(p.runs.load() == 1);
  CHECK(g.r->systems->last_error(id) == errc::job_failed);
  CHECK(g.r->systems->failures() == 1);
}
