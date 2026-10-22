// RT-INV-CMD-03 (SL-ARCH-003 ARCH3-R120, R119, ADR-0025): commands naming a stale entity are
// skipped and counted, and flush_report lists the applied operations in playback order. Also the
// other outcomes of flush: refused adds and removes are counted, a world out_of_memory and a
// recording failure stop playback with the commands of smaller keys applied, and wrong_thread,
// another registry, a local_seq other than 0 and a repeated base key are refused.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"

#include "../../../engine/runtime/testing/failing_resource.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

using op_kind = ecs::flush_report::op_kind;
using op = ecs::flush_report::op;

struct pos {
  std::int32_t x = 0;
  std::int32_t y = 0;
};
struct vel {
  std::int32_t v = 0;
};
struct unregistered {
  std::int32_t u = 0;
};

det::ordered_key key(std::uint16_t system, std::uint32_t chunk) {
  return det::ordered_key{
      .tick = 9, .phase = 4, .system_index = system, .chunk_index = chunk, .local_seq = 0};
}

struct rig {
  ecs::component_registry registry;
  mem::tracking_resource world_memory{"cmd.report.world"};
  mem::tracking_resource arena_memory{"cmd.report.arena"};
  std::unique_ptr<ecs::world> w;
  ecs::component_id pos_id;
  ecs::component_id vel_id;

  rig() {
    pos_id = registry.add<pos>("pos").value_or(ecs::component_id{});
    vel_id = registry.add<vel>("vel").value_or(ecs::component_id{});
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, world_memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }

  [[nodiscard]] ecs::entity make(std::int32_t x) const {
    auto e = w->create_with(pos{.x = x, .y = 0});
    REQUIRE(e.has_value());
    return e.value_or(ecs::entity{});
  }

  [[nodiscard]] std::int32_t x_of(ecs::entity e) const {
    auto p = w->get<pos>(e);
    return p ? (*p)->x : -1;
  }
};

} // namespace

TEST_CASE("RT-INV-CMD-03: commands on a stale entity are skipped and counted") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity dead = r.make(1);
  REQUIRE(r.w->destroy(dead).has_value());
  ecs::command_buffer &b = q.open(key(0, 0));
  b.destroy(dead);
  b.add(dead, vel{.v = 1});
  b.remove<pos>(dead);
  b.set(dead, pos{.x = 2, .y = 2});
  b.set(ecs::entity{}, pos{.x = 3, .y = 3}); // the null handle
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(report->skipped_stale == 5);
  CHECK(report->skipped_refused == 0);
  CHECK(report->applied.empty());
  CHECK(report->resolved.empty());
}

TEST_CASE("RT-INV-CMD-03: an entity destroyed during playback is stale for later commands") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e = r.make(1);
  ecs::command_buffer &first = q.open(key(0, 0));
  ecs::command_buffer &second = q.open(key(0, 1));
  second.set(e, pos{.x = 5, .y = 5}); // key (0, 1): after the destroy
  first.destroy(e);
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(report->applied ==
        std::vector<op>{{.kind = op_kind::destroy, .target = e, .component = ecs::component_id{}}});
  CHECK(report->skipped_stale == 1);
  CHECK_FALSE(r.w->is_alive(e));
}

TEST_CASE("RT-INV-CMD-03: flush_report lists the applied operations in playback order") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e1 = r.make(1);
  const ecs::entity e2 = r.make(2);
  // Opened out of key order: k2, k0, k1.
  ecs::command_buffer &b2 = q.open(key(3, 0));
  ecs::command_buffer &b0 = q.open(key(1, 4));
  ecs::command_buffer &b1 = q.open(key(1, 5));
  b2.set(e1, pos{.x = 30, .y = 0});
  b2.destroy(e2);
  b0.add(e1, vel{.v = 1});
  const ecs::pending_entity p = b0.spawn_with(pos{.x = 7, .y = 7});
  b1.set(e1, pos{.x = 10, .y = 0});
  b1.add(e1, vel{.v = 2}); // component_exists: refused
  b1.remove<vel>(e2);      // component_missing: refused
  b1.set(p, vel{.v = 3});  // after the spawn in key order
  b1.remove<pos>(e2);
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  REQUIRE(report->resolved.size() == 1);
  CHECK(report->resolved[0].first == p);
  const ecs::entity spawned = report->resolved[0].second;
  const std::vector<op> expected{
      {.kind = op_kind::add, .target = e1, .component = r.vel_id},
      {.kind = op_kind::spawn, .target = spawned, .component = ecs::component_id{}},
      {.kind = op_kind::set, .target = e1, .component = r.pos_id},
      {.kind = op_kind::set, .target = spawned, .component = r.vel_id, .added = true},
      {.kind = op_kind::remove, .target = e2, .component = r.pos_id},
      {.kind = op_kind::set, .target = e1, .component = r.pos_id},
      {.kind = op_kind::destroy, .target = e2, .component = ecs::component_id{}}};
  CHECK(report->applied == expected);
  CHECK(report->skipped_refused == 2);
  CHECK(report->skipped_stale == 0);
  CHECK(r.x_of(e1) == 30);
  CHECK_FALSE(r.w->is_alive(e2));
  auto v = r.w->get<vel>(spawned);
  REQUIRE(v.has_value());
  CHECK((*v)->v == 3);
}

TEST_CASE("RT-INV-CMD-03: a set that adds a missing component is still reported as set") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e = r.make(1);
  ecs::command_buffer &b = q.open(key(0, 0));
  b.set(e, vel{.v = 4}); // e lacks vel: added
  b.set(e, vel{.v = 5}); // a write
  b.set(e, pos{.x = 6, .y = 0});
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(report->applied ==
        std::vector<op>{{.kind = op_kind::set, .target = e, .component = r.vel_id, .added = true},
                        {.kind = op_kind::set, .target = e, .component = r.vel_id},
                        {.kind = op_kind::set, .target = e, .component = r.pos_id}});
  CHECK(report->skipped_refused == 0);
  auto v = r.w->get<vel>(e);
  REQUIRE(v.has_value());
  CHECK((*v)->v == 5);
  CHECK(r.x_of(e) == 6);
}

TEST_CASE("RT-INV-CMD-03: a world out_of_memory stops playback after the earlier commands") {
  // A dry run counts the world's allocations up to the flush; the real run fails the next one,
  // which is the archetype the add of vel needs.
  auto scenario = [](std::uint64_t fail_at, std::uint64_t &calls_before_flush) {
    rtt::failing_resource memory(*std::pmr::new_delete_resource(), fail_at);
    ecs::component_registry registry;
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    ecs::world &w = **made;
    std::vector<ecs::entity> es;
    for (std::int32_t i = 0; i < 3; ++i) {
      auto e = w.create_with(pos{.x = i, .y = 0});
      REQUIRE(e.has_value());
      es.push_back(e.value_or(ecs::entity{}));
    }
    mem::tracking_resource arena_memory{"cmd.report.oom"};
    mem::frame_arena arena(arena_memory, 0);
    ecs::command_queue q(registry, arena);
    ecs::command_buffer &b = q.open(key(0, 0));
    b.destroy(es[0]);
    b.add(es[1], vel{.v = 1});
    b.set(es[2], pos{.x = 50, .y = 0});
    calls_before_flush = memory.calls();
    auto report = q.flush(w);
    std::optional<sl::errc> code;
    if (!report) {
      code = report.error().code;
    }
    const bool e0_alive = w.is_alive(es[0]);
    const bool e1_vel = w.get<vel>(es[1]).has_value();
    auto p2 = w.get<pos>(es[2]);
    const std::int32_t x2 = p2 ? (*p2)->x : -1;
    return std::make_tuple(code, e0_alive, e1_vel, x2);
  };
  std::uint64_t before = 0;
  const auto [dry_code, dry_e0, dry_e1, dry_x2] = scenario(0, before);
  CHECK_FALSE(dry_code.has_value());
  CHECK_FALSE(dry_e0);
  CHECK(dry_e1);
  CHECK(dry_x2 == 50);
  std::uint64_t again = 0;
  const auto [code, e0_alive, e1_vel, x2] = scenario(before + 1, again);
  CHECK(again == before);
  REQUIRE(code.has_value());
  CHECK(code.value_or(sl::errc::ok) == sl::errc::out_of_memory);
  CHECK_FALSE(e0_alive); // the destroy before it applied
  CHECK_FALSE(e1_vel);   // the failed add changed nothing
  CHECK(x2 == 2);        // the set after it did not run
}

TEST_CASE("RT-INV-CMD-03: a recording failure stops playback at its key") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e0 = r.make(0);
  const ecs::entity e1 = r.make(1);
  const ecs::entity e2 = r.make(2);
  ecs::command_buffer &mid = q.open(key(1, 0));
  ecs::command_buffer &late = q.open(key(2, 0));
  ecs::command_buffer &early = q.open(key(0, 0));
  mid.set(e1, pos{.x = 11, .y = 0});
  mid.add(e1, unregistered{.u = 1});
  mid.set(e1, pos{.x = 12, .y = 0}); // not recorded: the buffer failed
  CHECK(mid.status() == sl::errc::component_unknown);
  CHECK(mid.size() == 1);
  late.set(e2, pos{.x = 22, .y = 0});
  early.set(e0, pos{.x = 10, .y = 0});
  auto report = q.flush(*r.w);
  REQUIRE_FALSE(report.has_value());
  CHECK(report.error().code == sl::errc::component_unknown);
  CHECK(r.x_of(e0) == 10);
  CHECK(r.x_of(e1) == 11);
  CHECK(r.x_of(e2) == 2);
  CHECK(q.open_count() == 0);
}

TEST_CASE("RT-INV-CMD-03: a frame arena out_of_memory while recording is returned at flush") {
  rig r;
  rtt::failing_resource upstream(*std::pmr::new_delete_resource(), 1);
  mem::frame_arena arena(upstream, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e = r.make(4);
  ecs::command_buffer &b = q.open(key(0, 0));
  b.set(e, pos{.x = 40, .y = 0});
  CHECK(b.status() == sl::errc::out_of_memory);
  CHECK(b.size() == 0);
  auto report = q.flush(*r.w);
  REQUIRE_FALSE(report.has_value());
  CHECK(report.error().code == sl::errc::out_of_memory);
  CHECK(r.x_of(e) == 4);
}

TEST_CASE("RT-INV-CMD-03: misuse is refused before anything is applied") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  const ecs::entity e = r.make(1);

  SUBCASE("a base key with a local_seq other than 0") {
    det::ordered_key k = key(0, 0);
    k.local_seq = 3;
    ecs::command_buffer &b = q.open(k);
    CHECK(b.status() == sl::errc::invalid_argument);
    b.set(e, pos{.x = 9, .y = 9});
    auto report = q.flush(*r.w);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == sl::errc::invalid_argument);
  }
  SUBCASE("two buffers of one base key") {
    q.open(key(0, 0)).set(e, pos{.x = 9, .y = 9});
    q.open(key(0, 0)).set(e, pos{.x = 8, .y = 8});
    auto report = q.flush(*r.w);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == sl::errc::invalid_argument);
  }
  SUBCASE("a world of another registry") {
    ecs::component_registry other;
    REQUIRE(other.freeze().has_value());
    mem::tracking_resource other_memory{"cmd.report.other"};
    auto made = ecs::world::make(other, other_memory);
    REQUIRE(made.has_value());
    q.open(key(0, 0)).set(e, pos{.x = 9, .y = 9});
    auto report = q.flush(**made);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == sl::errc::invalid_argument);
    q.clear();
  }
  SUBCASE("flush off the owner thread") {
    q.open(key(0, 0)).set(e, pos{.x = 9, .y = 9});
    std::optional<sl::errc> code;
    std::thread other([&] {
      auto report = q.flush(*r.w);
      code = report ? sl::errc::ok : report.error().code;
    });
    other.join();
    CHECK(code.value_or(sl::errc::ok) == sl::errc::wrong_thread);
    CHECK(q.open_count() == 1);
    q.clear();
  }
  CHECK(q.open_count() == 0);
  CHECK(r.x_of(e) == 1);
  // The queue stays usable.
  q.open(key(5, 5)).set(e, pos{.x = 6, .y = 6});
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(r.x_of(e) == 6);
}
