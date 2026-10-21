// RT-INV-STORE-07 (SL-ARCH-003 ARCH3-R109, ADR-0025, `thread_safety`): structural changes happen
// only at boundaries on the owner thread. Inside a phase every structural call is
// structural_change_forbidden; while an iteration guard is held it trips the Debug assertion
// (captured here) and is refused; empty chunks stay with their archetype until
// release_empty_chunks returns them to the pool, which later archetypes reuse without a new slab;
// another thread gets wrong_thread; a background job touching the world trips the Debug assertion.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

#ifdef NDEBUG
constexpr bool debug_asserts = false;
#else
constexpr bool debug_asserts = true;
#endif

struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"store.guard"};
  std::unique_ptr<ecs::world> w;
};

} // namespace

TEST_CASE("RT-INV-STORE-07: a structural call inside a phase is structural_change_forbidden") {
  fixture f;
  auto e = f.w->create_with(rtt::gen_health{.hp = 5});
  REQUIRE(e.has_value());
  const auto version = f.w->version();
  f.w->set_phase_active(true);
  CHECK(f.w->phase_active());
  constexpr sl::errc forbidden = sl::errc::structural_change_forbidden;
  CHECK(f.w->create().error().code == forbidden);
  CHECK(f.w->create_with(rtt::gen_tag{}).error().code == forbidden);
  CHECK(f.w->destroy(*e).error().code == forbidden);
  CHECK(f.w->add<rtt::gen_tag>(*e, rtt::gen_tag{}).error().code == forbidden);
  CHECK(f.w->remove<rtt::gen_health>(*e).error().code == forbidden);
  CHECK(f.w->set<rtt::gen_health>(*e, rtt::gen_health{.hp = 6}).error().code == forbidden);
  CHECK(f.w->restore_from(f.w->capture_image()).error().code == forbidden);
  CHECK((*f.w->get<rtt::gen_health>(*e))->hp == 5); // reads and in-place writes stay allowed
  (*f.w->get<rtt::gen_health>(*e))->hp = 8;
  CHECK(f.w->version() == version);
  f.w->set_phase_active(false);
  CHECK(f.w->add<rtt::gen_tag>(*e, rtt::gen_tag{}).has_value());
  CHECK((*f.w->get<rtt::gen_health>(*e))->hp == 8);
}

TEST_CASE("RT-INV-STORE-07: a structural call during iteration trips the captured guard") {
  fixture f;
  auto e = f.w->create_with(rtt::gen_position{});
  REQUIRE(e.has_value());
  int asserts = 0;
  std::string message;
  {
    const sl::scoped_assert_handler handler([&](const sl::assert_report &report) {
      ++asserts;
      message = std::string(report.message);
    });
    {
      const ecs::iteration_guard guard = f.w->begin_iteration();
      CHECK(f.w->create().error().code == sl::errc::structural_change_forbidden);
      CHECK(f.w->destroy(*e).error().code == sl::errc::structural_change_forbidden);
      CHECK(f.w->is_alive(*e));
    }
    CHECK(f.w->create().has_value()); // the guard ended
  }
  if (!debug_asserts) {
    MESSAGE("skipped: SL_DEBUG_ASSERT is compiled out (NDEBUG)");
    CHECK(asserts == 0);
    return;
  }
  CHECK(asserts == 2);
  CHECK_FALSE(message.empty());
}

TEST_CASE("RT-INV-STORE-07: empty chunks return to the pool at release_empty_chunks") {
  fixture f;
  std::vector<ecs::entity> es;
  for (int i = 0; i < 2048; ++i) {
    auto e = f.w->create();
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  CHECK(f.w->stats().chunks == 2);
  CHECK(f.w->stats().slabs == 1);
  CHECK(f.w->stats().pooled_chunks == ecs::chunks_per_slab - 2);
  for (const ecs::entity e : es) {
    REQUIRE(f.w->destroy(e).has_value());
  }
  CHECK(f.w->stats().rows == 0);
  CHECK(f.w->stats().chunks == 2); // kept until the boundary
  CHECK(f.w->stats().empty_chunks == 2);
  f.w->release_empty_chunks();
  CHECK(f.w->stats().chunks == 0);
  CHECK(f.w->stats().empty_chunks == 0);
  CHECK(f.w->stats().pooled_chunks == ecs::chunks_per_slab);
  const auto slab_bytes = f.memory.totals().live_bytes;
  for (int i = 0; i < 700; ++i) {
    REQUIRE(f.w->create_with(rtt::gen_large{}).has_value()); // 7 a chunk: 100 chunks
  }
  CHECK(f.w->stats().chunks == 100);
  CHECK(f.w->stats().slabs == 1); // the released chunks were reused
  CHECK(f.memory.totals().live_bytes >= slab_bytes);
  CHECK(f.memory.totals().live_bytes < slab_bytes + ecs::chunk_pool_slab_bytes);
}

TEST_CASE("RT-INV-STORE-07: a structural call off the owner thread is wrong_thread") {
  fixture f;
  auto e = f.w->create();
  REQUIRE(e.has_value());
  std::optional<sl::errc> create_code;
  std::optional<sl::errc> add_code;
  std::thread other([&] {
    auto c = f.w->create();
    create_code = c ? std::nullopt : std::optional(c.error().code);
    auto a = f.w->add<rtt::gen_tag>(*e, rtt::gen_tag{});
    add_code = a ? std::nullopt : std::optional(a.error().code);
  });
  other.join();
  CHECK(create_code == sl::errc::wrong_thread);
  CHECK(add_code == sl::errc::wrong_thread);
  CHECK(f.w->entities().alive() == 1);
}

TEST_CASE("RT-INV-STORE-07: a background job touching the world trips the captured guard") {
  fixture f;
  auto e = f.w->create_with(rtt::gen_health{.hp = 4});
  REQUIRE(e.has_value());
  rtt::scripted_platform plat;
  jobs::job_config config;
  config.workers = 1;
  auto js = jobs::job_system::start(plat.get(), std::move(config));
  REQUIRE(js.has_value());
  f.w->set_job_system(js->get());
  std::atomic<int> asserts{0};
  std::atomic<int> seen_hp{0};
  {
    const sl::scoped_assert_handler handler(
        [&](const sl::assert_report & /*report*/) { asserts.fetch_add(1); });
    const ecs::world &w = *f.w;
    const ecs::entity target = *e;
    const auto read = [&w, &seen_hp, target](jobs::job_context & /*ctx*/) noexcept {
      auto hp = w.get<rtt::gen_health>(target);
      seen_hp.store(hp ? (*hp)->hp : -1);
    };
    const jobs::job_handle frame =
        (*js)->submit({.fn = read, .prio = jobs::priority::frame, .name = "store.frame"});
    REQUIRE((*js)->wait(frame));
    CHECK(asserts.load() == 0); // frame jobs may read
    const jobs::job_handle background =
        (*js)->submit({.fn = read, .prio = jobs::priority::background, .name = "store.background"});
    REQUIRE((*js)->wait(background));
  }
  f.w->set_job_system(nullptr);
  CHECK(seen_hp.load() == 4);
  if (!debug_asserts) {
    MESSAGE("skipped: SL_DEBUG_ASSERT is compiled out (NDEBUG)");
    CHECK(asserts.load() == 0);
    return;
  }
  CHECK(asserts.load() == 1);
}
