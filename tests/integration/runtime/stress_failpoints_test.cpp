// RT-INV-STRESS-08 (SL-ARCH-003 ARCH3-R020, `failpoints` RFP0-RFP10, ADR-0022): every failpoint
// is visited by its workload (rt::visits grows over an unarmed run) and, armed through
// testing::arm, its outcome matches the failpoints table:
//   RFP0  out_of_memory; the tracking resource and the entity allocator are unchanged;
//   RFP1  thread_spawn_failed; runtime::start unwinds to Created and a later start succeeds;
//   RFP2  job_failed, the dependent job_cancelled, the job system keeps running jobs;
//   RFP3  job_failed inside the frame, the worker continues, Faulted at end_frame;
//   RFP4  seeded stalls of 0 to 2000 us leave every tick hash of sim_fixture unchanged;
//   RFP5  the system fails, the frame ends Faulted;
//   RFP6  a backward sample gives delta 0 and non_monotonic + 1; a hitch is clamped to
//         max_frame_delta with the excess in dropped_ns;
//   RFP7  the channel behaves as full: the newest event is dropped and counted;
//   RFP8  save fails as a simulated crash before the write; after a crash the old snapshot
//         loads, and vfs crashes after every primitive leave the old or the new one;
//   RFP9  request_suspend at a seeded visit; Suspended at an end_frame, and the state hash at the
//         target tick equals the unsuspended run;
//   RFP10 forced entity_capacity changes nothing; param 1 saturates a generation so the slot
//         retires on destroy.
// Every case is skipped when the failpoints are compiled out (Release).

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"
#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/sim_fixture.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;
namespace ft = sl::fabric::testing;
using std::chrono::milliseconds;

constexpr const char *test_name = "runtime_stress_failpoints_test";
constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t workers = 2;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

bool skipped() {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
    return true;
  }
  rtt::disarm_all();
  return false;
}

struct counter {
  std::uint64_t value = 0;
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept {
      c.value = (c.value * 6364136223846793005ULL) + 1442695040888963407ULL;
    });
  }
};

// A runtime (2 workers, 50 Hz, leak_check on, policy stop) on a scripted platform with a counter
// world and its step system.
struct rig {
  explicit rig(std::uint32_t instance) : plat(instance) {
    rt::runtime_config c;
    c.instance_id = instance;
    c.workers = workers;
    c.rate = {.num = 50, .den = 1};
    c.seed = 0x5EED0990018ULL;
    c.leak_check = true;
    auto made = rt::runtime::create(plat.get(), c);
    REQUIRE(made.has_value());
    r = std::move(*made);
    rt::type_builder<counter> b("stress.flt.counter");
    SL_RT_FIELD(b, counter, value, kept);
    auto t = r->types().add(b.build());
    REQUIRE(t.has_value());
    REQUIRE(r->components().add<counter>("stress.flt.counter", *t).has_value());
    auto q = ecs::query<ecs::write<counter>>::make(r->components());
    REQUIRE(q.has_value());
    ecs::access_set access;
    ecs::add_query_access(access, q->access());
    const ecs::component_access needed = q->access();
    REQUIRE(r->add_system(ecs::system_desc{.name = "stress.flt.step",
                                           .ph = rt::phase::tick,
                                           .access = std::move(access),
                                           .before = {},
                                           .after = {},
                                           .fn = step_body{.q = std::move(*q)},
                                           .chunked = false,
                                           .queries = {needed}})
                .has_value());
  }

  void start(std::uint32_t entities = 64) const {
    REQUIRE(r->start().has_value());
    for (std::uint32_t i = 0; i < entities; ++i) {
      REQUIRE(r->world().create_with(counter{.value = std::uint64_t{i} * 977U}).has_value());
    }
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }

  // Frames until the running tick is `t`, resuming a suspended runtime.
  void run_to(rt::time::tick t, std::uint32_t &suspensions) {
    for (int guard = 0; guard < 1000 && now() < t; ++guard) {
      if (r->state() == rt::rt_state::suspended) {
        ++suspensions;
        REQUIRE(r->resume().has_value());
      }
      REQUIRE(frame().has_value());
    }
    REQUIRE(now() == t);
  }

  [[nodiscard]] sl::fabric::digest hash() const {
    auto h = rt::state_hash(r->world(), r->components(), r->types(),
                            std::span<const det::snapshot_contributor *const>{});
    REQUIRE(h.has_value());
    return *h;
  }

  rtt::scripted_platform plat;
  std::unique_ptr<rt::runtime> r;
};

struct flood {
  std::uint32_t n = 0;
};

rt::type_info flood_info() {
  rt::type_builder<flood> b("stress.flt.flood");
  SL_RT_FIELD(b, flood, n, kept);
  return b.build();
}

} // namespace

TEST_CASE("RT-INV-STRESS-08: RFP0 alloc.fail gives out_of_memory and leaves the object unchanged") {
  if (skipped()) {
    return;
  }
  mem::tracking_resource r("stress.flt.rfp0");
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp0);
  auto warm = r.try_allocate(256, 16, r.default_tag());
  REQUIRE(warm.has_value());
  CHECK(rt::visits(rt::failpoint::rfp0) > v0);
  const mem::tag_counters before = r.totals();
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp0, 1);
    REQUIRE(arm.status().has_value());
    auto failed = r.try_allocate(128, 8, r.default_tag());
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::out_of_memory);
  }
  CHECK(r.totals().live_bytes == before.live_bytes);
  CHECK(r.totals().allocs == before.allocs);
  // A structural operation: the first create allocates the first page and fails whole.
  ecs::entity_allocator a(ecs::entity_page_records * 2, r);
  const ecs::entity_allocator::state empty = a.capture();
  const std::uint64_t live = r.totals().live_bytes;
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp0, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create();
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == sl::errc::out_of_memory);
  }
  CHECK(a.capture() == empty);
  CHECK(r.totals().live_bytes == live);
  CHECK(a.create().has_value()); // disarmed: the same create lands
  r.release(*warm, 256, 16);
}

TEST_CASE("RT-INV-STRESS-08: RFP1 thread.spawn gives thread_spawn_failed and start unwinds to "
          "Created") {
  if (skipped()) {
    return;
  }
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990028ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  std::uint64_t state = seed;
  {
    rig g(1101);
    const std::uint64_t v0 = rt::visits(rt::failpoint::rfp1);
    g.start();
    CHECK(rt::visits(rt::failpoint::rfp1) > v0);
    REQUIRE(g.r->shutdown().has_value());
  }
  for (std::uint32_t round = 0; round < 4; ++round) {
    const std::uint64_t k = 1 + (splitmix64(state) % workers);
    INFO("RFP1 at spawn " << k);
    rig g(1102 + round);
    REQUIRE(rtt::failing_thread_factory::arm_rfp1(k).has_value());
    const auto started = g.r->start();
    rtt::disarm(rt::failpoint::rfp1);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code == sl::errc::thread_spawn_failed);
    CHECK(g.r->state() == rt::rt_state::created);
    g.start(); // no thread was left behind: a second start spawns them all
    CHECK(g.frame().has_value());
    REQUIRE(g.r->shutdown().has_value()); // leak_check on
  }
}

TEST_CASE("RT-INV-STRESS-08: RFP2 job.error fails the job with job_failed and cancels its "
          "dependent") {
  if (skipped()) {
    return;
  }
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = workers;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  std::array<std::atomic<int>, 4> ran{};
  const auto mark = [&ran](std::size_t i) {
    return jobs::job_desc{
        .fn = [&ran, i](jobs::job_context & /*ctx*/) noexcept { ran.at(i).fetch_add(1); },
        .name = "stress.flt.mark"};
  };
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp2);
  REQUIRE((*js)->wait((*js)->submit(mark(0))).has_value());
  CHECK(rt::visits(rt::failpoint::rfp2) > v0);
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp2, 1);
    REQUIRE(arm.status().has_value());
    const jobs::job_handle failing = (*js)->submit(mark(1));
    const jobs::job_handle dep = (*js)->submit(mark(2), std::array{jobs::dependency::on(failing)});
    auto r = (*js)->wait(failing);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::job_failed);
    auto d = (*js)->wait(dep);
    REQUIRE_FALSE(d.has_value());
    CHECK(d.error().code == sl::errc::job_cancelled);
  }
  CHECK(ran.at(1).load() == 0);
  CHECK(ran.at(2).load() == 0);
  REQUIRE((*js)->wait((*js)->submit(mark(3))).has_value()); // the system is unchanged
  CHECK(ran.at(3).load() == 1);
  REQUIRE((*js)->stop().has_value());
}

TEST_CASE("RT-INV-STRESS-08: RFP3 job.fatal and RFP5 system.fail fault the frame at end_frame") {
  if (skipped()) {
    return;
  }
  std::uint32_t id = 1110;
  for (const rt::failpoint point : {rt::failpoint::rfp3, rt::failpoint::rfp5}) {
    INFO((point == rt::failpoint::rfp3 ? "RFP3" : "RFP5"));
    rig g(++id);
    g.start();
    const std::uint64_t v0 = rt::visits(point);
    for (int i = 0; i < 4; ++i) {
      REQUIRE(g.frame().has_value());
    }
    // cppcheck-suppress knownConditionTrueFalse ; visits() is 0 only when compiled out
    CHECK(rt::visits(point) > v0);
    const std::uint32_t alive = g.r->world().entities().alive();
    sl::result<rt::frame_stats> ran;
    {
      const rtt::scoped_arm arm(point, 1);
      REQUIRE(arm.status().has_value());
      ran = g.frame();
    }
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::runtime_faulted);
    CHECK(g.r->frame_faulted());
    CHECK(g.r->world().entities().alive() == alive);
    const rt::rt_state s = g.r->state();
    CHECK(
        (s == rt::rt_state::faulted || s == rt::rt_state::stopping || s == rt::rt_state::stopped));
    REQUIRE(g.r->shutdown().has_value());
  }
}

TEST_CASE("RT-INV-STRESS-08: RFP4 worker.stall stalls of 0 to 2000 us leave the tick hashes "
          "unchanged") {
  if (skipped()) {
    return;
  }
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990048ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const auto ticks = static_cast<std::uint32_t>(rtt::scaled(10));
  const rtt::sim_config config{.workers = 4,
                               .policy = ecs::execution_policy::parallel,
                               .ticks = ticks,
                               .seed = 0x5EED0990004ULL,
                               .entities = 1000};
  auto plain = rtt::sim_fixture::make(config);
  REQUIRE(plain.has_value());
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp4);
  auto expected = (*plain)->run(ticks);
  REQUIRE(expected.has_value());
  CHECK(rt::visits(rt::failpoint::rfp4) > v0);
  auto stalled = rtt::sim_fixture::make(config);
  REQUIRE(stalled.has_value());
  std::uint64_t state = seed;
  std::vector<sl::fabric::digest> got;
  for (std::uint32_t t = 0; t < ticks; ++t) {
    const std::uint64_t at_visit = 1 + (splitmix64(state) % 32U);
    const auto stall_us = static_cast<std::uint32_t>(splitmix64(state) % 2001U);
    REQUIRE(rtt::arm(rt::failpoint::rfp4, at_visit, false, stall_us).has_value());
    auto h = (*stalled)->run(1);
    REQUIRE(h.has_value());
    REQUIRE(h->size() == 1);
    got.push_back(h->front());
  }
  rtt::disarm(rt::failpoint::rfp4);
  CHECK(got == *expected);
}

TEST_CASE("RT-INV-STRESS-08: RFP6 clock.jump gives delta 0 backwards and clamps a hitch") {
  if (skipped()) {
    return;
  }
  rt::time::frame_clock clock;
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp6);
  CHECK(clock.sample(milliseconds(0)) == rt::nanos{0});
  CHECK(clock.sample(milliseconds(20)) == milliseconds(20));
  CHECK(rt::visits(rt::failpoint::rfp6) > v0);
  const std::uint64_t backward = clock.non_monotonic();
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp6, 1);
    REQUIRE(arm.status().has_value());
    CHECK(clock.sample(milliseconds(40)) == rt::nanos{0});
  }
  CHECK(clock.non_monotonic() == backward + 1);
  CHECK(clock.sample(milliseconds(60)) == milliseconds(40)); // the baseline did not move
  const rt::nanos dropped = clock.dropped_ns();
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp6, 1, false, 1000);
    REQUIRE(arm.status().has_value());
    CHECK(clock.sample(milliseconds(80)) == rt::time::max_frame_delta_default);
  }
  CHECK(clock.dropped_ns() - dropped == milliseconds(1020) - rt::time::max_frame_delta_default);
  CHECK(clock.sample(milliseconds(100)) == milliseconds(20)); // the hitch is per frame
}

TEST_CASE("RT-INV-STRESS-08: RFP7 event.overflow drops and counts the newest event") {
  if (skipped()) {
    return;
  }
  rt::event_registry reg;
  REQUIRE(reg.add<flood>("stress.flt.flood", flood_info()).has_value());
  REQUIRE(reg.freeze().has_value());
  auto made = rt::event_channel<flood>::make(reg);
  REQUIRE(made.has_value());
  rt::event_channel<flood> &ch = *made;
  const rt::writer_key key{.tick = 1, .ph = rt::phase::tick, .system_index = 0, .chunk_index = 0};
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp7);
  REQUIRE(ch.write(key, flood{.n = 0}).has_value());
  CHECK(rt::visits(rt::failpoint::rfp7) > v0);
  const std::uint64_t dropped = ch.dropped();
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp7, 1);
    REQUIRE(arm.status().has_value());
    REQUIRE(ch.write(key, flood{.n = 1}).has_value()); // dropped
  }
  CHECK(ch.dropped() == dropped + 1);
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp7, 1);
    REQUIRE(arm.status().has_value());
    const auto tried = ch.try_write(key, flood{.n = 2});
    REQUIRE_FALSE(tried.has_value());
    CHECK(tried.error().code == sl::errc::event_overflow);
  }
  CHECK(ch.dropped() == dropped + 2);
  REQUIRE(ch.write(key, flood{.n = 3}).has_value());
  ch.stream().deliver(1);
  std::vector<std::uint32_t> got;
  ch.read([&got](const rt::event_envelope & /*env*/, const flood &ev) { got.push_back(ev.n); });
  CHECK(got == std::vector<std::uint32_t>{0, 3});
  ch.stream().end_frame();
}

TEST_CASE("RT-INV-STRESS-08: RFP8 snapshot.io is a simulated crash and the old or the new "
          "snapshot stays loadable") {
  if (skipped()) {
    return;
  }
  constexpr const char *file = "stress.slrs";
  rig g(1120);
  g.start(256);
  REQUIRE(g.frame().has_value());
  const auto take = [&g](std::uint64_t tick) {
    rt::snapshot_meta meta;
    meta.frame = tick;
    meta.tick = tick;
    auto s = rt::take(g.r->world(), g.r->components(), g.r->types(), {}, meta);
    REQUIRE(s.has_value());
    return std::move(*s);
  };
  const rt::snapshot old_s = take(1);
  REQUIRE(g.frame().has_value());
  REQUIRE(g.frame().has_value());
  const rt::snapshot new_s = take(3);
  REQUIRE_FALSE(std::ranges::equal(old_s.bytes(), new_s.bytes()));
  std::uint64_t primitives = 0;
  {
    ft::faulty_vfs fs;
    const rt::file_access files(fs, "/");
    const std::uint64_t v0 = rt::visits(rt::failpoint::rfp8);
    REQUIRE(rt::save(files, file, old_s).has_value());
    CHECK(rt::visits(rt::failpoint::rfp8) > v0);
    const std::uint64_t before = fs.primitives();
    {
      const rtt::scoped_arm arm(rt::failpoint::rfp8, 1);
      REQUIRE(arm.status().has_value());
      auto failed = rt::save(files, file, new_s);
      REQUIRE_FALSE(failed.has_value());
      CHECK(failed.error().code == sl::errc::io_error);
    }
    CHECK(fs.primitives() == before);
    REQUIRE(fs.crash(ft::crash_policy::drop_unsynced).has_value());
    auto loaded = rt::load(files, file);
    REQUIRE(loaded.has_value());
    CHECK(std::ranges::equal(loaded->bytes(), old_s.bytes()));
    const std::uint64_t at = fs.primitives();
    REQUIRE(rt::save(files, file, new_s).has_value());
    primitives = fs.primitives() - at;
  }
  REQUIRE(primitives >= 4);
  // A crash after every primitive of the new save: the old or the new snapshot loads.
  for (std::uint64_t k = 0; k <= primitives; ++k) {
    INFO("crash after primitive " << k);
    ft::faulty_vfs fs;
    const rt::file_access files(fs, "/");
    REQUIRE(rt::save(files, file, old_s).has_value());
    fs.fail_after(k);
    const bool saved = rt::save(files, file, new_s).has_value();
    REQUIRE(fs.crash(ft::crash_policy::drop_unsynced).has_value());
    fs.disarm();
    auto loaded = rt::load(files, file);
    REQUIRE(loaded.has_value());
    const bool is_old = std::ranges::equal(loaded->bytes(), old_s.bytes());
    const bool is_new = std::ranges::equal(loaded->bytes(), new_s.bytes());
    CHECK((is_old || is_new));
    CHECK((!saved || is_new));
  }
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-STRESS-08: RFP9 suspend.request suspends at end_frame and the hash is "
          "unchanged") {
  if (skipped()) {
    return;
  }
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990098ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  constexpr rt::time::tick target = 20;
  sl::fabric::digest expected{};
  {
    rig g(1130);
    g.start();
    std::uint32_t suspensions = 0;
    const std::uint64_t v0 = rt::visits(rt::failpoint::rfp9);
    g.run_to(target, suspensions);
    CHECK(rt::visits(rt::failpoint::rfp9) > v0);
    CHECK(suspensions == 0);
    expected = g.hash();
    REQUIRE(g.r->shutdown().has_value());
  }
  std::uint64_t state = seed;
  for (std::uint32_t round = 0; round < 4; ++round) {
    const std::uint64_t k = 1 + (splitmix64(state) % 16U);
    INFO("RFP9 at visit " << k);
    rig g(1131 + round);
    g.start();
    std::uint32_t suspensions = 0;
    g.run_to(5, suspensions);
    {
      const rtt::scoped_arm arm(rt::failpoint::rfp9, k);
      REQUIRE(arm.status().has_value());
      for (int i = 0; i < 4 && g.r->state() == rt::rt_state::running; ++i) {
        REQUIRE(g.frame().has_value());
      }
    }
    CHECK(g.r->state() == rt::rt_state::suspended);
    g.run_to(target, suspensions);
    CHECK(suspensions == 1);
    CHECK(g.hash() == expected);
    REQUIRE(g.r->shutdown().has_value());
  }
}

TEST_CASE("RT-INV-STRESS-08: RFP10 entity.exhaust forces entity_capacity or saturates a "
          "generation") {
  if (skipped()) {
    return;
  }
  constexpr std::uint32_t page = ecs::entity_page_records;
  mem::tracking_resource r("stress.flt.rfp10");
  ecs::entity_allocator a(page * 2, r);
  const std::uint64_t v0 = rt::visits(rt::failpoint::rfp10);
  REQUIRE(a.create().has_value());
  CHECK(rt::visits(rt::failpoint::rfp10) > v0);
  const ecs::entity_allocator::state before = a.capture();
  const std::uint64_t live = r.totals().live_bytes;
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create();
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == sl::errc::entity_capacity);
  }
  CHECK(a.capture() == before);
  CHECK(r.totals().live_bytes == live);
  ecs::entity doomed{};
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1, false, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create();
    REQUIRE(e.has_value());
    doomed = *e;
  }
  CHECK(a.is_alive(doomed));
  REQUIRE(a.destroy(doomed).has_value());
  CHECK_FALSE(a.is_alive(doomed));
  CHECK(a.retired_count() == 1);
  CHECK(a.destroy(doomed).error().code == sl::errc::stale_entity);
}
