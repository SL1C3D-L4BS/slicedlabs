// RT-INV-LIFE-02 (SL-ARCH-003 ARCH3-R025, ADR-0028): startup failing at each step (RFP1 at worker
// k, RFP0 at the memory root) leaves zero threads, zero tracked bytes and state Created. Threads
// are counted in /proc/self/task against the count before the runtime existed; tracked bytes are
// checked through the tracking resources' destruction reports (a live block logs a runtime.mem
// error) and the empty memory report. RFP1 fires at each worker spawn k (and the watchdog's in
// SL_RUNTIME_FAULTS builds), thread spawn k fails through the scripted thread factory, and RFP0
// fires at each tracked allocation of start from the root block on; after every failure a retry
// of start on the same runtime succeeds. The sweep (WO-109) counts start's RFP0 visits and fails
// each one start can unwind from: the root block (visit 1) and the world's location page table
// (visit 3). Visit 2, the entity allocator's page list, goes through std::pmr, where out_of_memory
// is fatal by design (ADR-0023, WO-088), and the job system allocates nothing tracked; both limits
// are recorded in SL-ARCH-003 ARCH3-R025.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/runtime.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t workers = 3;

struct counter {
  std::uint64_t value = 0;
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

class attached {
public:
  explicit attached(std::shared_ptr<sl::log::sink> sink) : sink_(std::move(sink)) {
    sl::log::default_logger().add_sink(sink_);
  }
  attached(const attached &) = delete;
  attached(attached &&) = delete;
  attached &operator=(const attached &) = delete;
  attached &operator=(attached &&) = delete;
  ~attached() { sl::log::default_logger().remove_sink(sink_.get()); }

private:
  std::shared_ptr<sl::log::sink> sink_;
};

std::size_t thread_count() {
  std::size_t n = 0;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
    static_cast<void>(entry);
    ++n;
  }
  return n;
}

/// The thread count with no runtime alive. It is read while a probe thread runs, so the sanitizer's
/// background thread (TSan starts it with the process's first thread) is already in it.
std::size_t idle_thread_count() {
  std::atomic<bool> release{false};
  std::thread probe([&release] {
    while (!release.load()) {
      std::this_thread::yield();
    }
  });
  const std::size_t running = thread_count();
  release.store(true);
  probe.join();
  return running - 1;
}

/// True once /proc/self/task shows `want` threads (a joined thread leaves it shortly after).
bool settles_at(std::size_t want) {
  for (int i = 0; i < 2000; ++i) {
    if (thread_count() == want) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

std::size_t live_block_reports(const sl::log::memory_sink &sink) {
  std::size_t n = 0;
  for (const auto &rec : sink.records()) {
    if (rec.system == "runtime.mem" && rec.level == sl::log::level::error) {
      // cppcheck-suppress useStlAlgorithm ; the two-field match reads plainer as a loop
      ++n;
    }
  }
  return n;
}

struct rig {
  explicit rig(std::uint32_t id) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = workers;
    cfg.rate = {.num = 50, .den = 1};
    cfg.snapshot_every = 4;
    cfg.leak_check = true;
    auto made = rt::runtime::create(plat.get(), cfg);
    REQUIRE(made.has_value());
    r = std::move(*made);
    rt::type_builder<counter> b("life.counter");
    SL_RT_FIELD(b, counter, value, kept);
    auto t = r->types().add(b.build());
    REQUIRE(t.has_value());
    REQUIRE(r->components().add<counter>("life.counter", *t).has_value());
    auto q = ecs::query<ecs::write<counter>>::make(r->components());
    REQUIRE(q.has_value());
    ecs::access_set access;
    ecs::add_query_access(access, q->access());
    const ecs::component_access needed = q->access();
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.step",
                                           .ph = rt::phase::tick,
                                           .access = std::move(access),
                                           .before = {},
                                           .after = {},
                                           .fn = step_body{.q = std::move(*q)},
                                           .chunked = false,
                                           .queries = {needed}})
                .has_value());
  }

  rtt::scripted_platform plat;
  std::unique_ptr<rt::runtime> r;
};

/// The state after a failed start: Created, no runtime thread, no live tracked block, no report.
void check_unwound(rig &g, std::size_t baseline, const sl::log::memory_sink &sink) {
  CHECK(g.r->state() == rt::rt_state::created);
  CHECK(settles_at(baseline));
  CHECK(live_block_reports(sink) == 0);
  CHECK(g.r->memory_report().tags.empty());
}

/// A retry of start on the same runtime succeeds and runs; shutdown finds nothing live.
void check_retry(rig &g, std::size_t baseline) {
  REQUIRE(g.r->start().has_value());
  CHECK(thread_count() > baseline);
  for (int i = 0; i < 5; ++i) {
    g.plat.clock().advance(std::chrono::milliseconds(20));
    REQUIRE(g.r->run_frame().has_value());
  }
  REQUIRE(g.r->shutdown().has_value());
  CHECK(settles_at(baseline));
}

} // namespace

TEST_CASE("RT-INV-LIFE-02: a thread spawn failing at worker k unwinds to Created") {
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  bool succeeded = false;
  for (std::uint64_t k = 1; k <= workers + 2 && !succeeded; ++k) {
    INFO("spawn " << k);
    rig g(200 + static_cast<std::uint32_t>(k));
    g.plat.threads().fail_at(k);
    const auto started = g.r->start();
    if (started) {
      // k is past the last spawn of start: every spawn before it was tested
      CHECK(k > workers);
      REQUIRE(g.r->shutdown().has_value());
      succeeded = true;
      break;
    }
    CHECK(started.error().code == sl::errc::thread_spawn_failed);
    check_unwound(g, baseline, *sink);
    g.plat.threads().fail_at(0);
    check_retry(g, baseline);
  }
  CHECK(succeeded);
  CHECK(live_block_reports(*sink) == 0);
}

TEST_CASE("RT-INV-LIFE-02: RFP1 at worker k unwinds to Created") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out");
    return;
  }
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  std::uint64_t failed = 0;
  for (std::uint64_t k = 1; k <= workers + 2; ++k) {
    INFO("RFP1 visit " << k);
    rig g(210 + static_cast<std::uint32_t>(k));
    REQUIRE(rtt::failing_thread_factory::arm_rfp1(k).has_value());
    const auto started = g.r->start();
    rtt::disarm(rt::failpoint::rfp1);
    if (started) {
      REQUIRE(g.r->shutdown().has_value());
      break;
    }
    ++failed;
    CHECK(started.error().code == sl::errc::thread_spawn_failed);
    check_unwound(g, baseline, *sink);
    check_retry(g, baseline);
  }
  // every worker spawn (and the watchdog's) failed once
  CHECK(failed >= workers);
  CHECK(live_block_reports(*sink) == 0);
}

TEST_CASE("RT-INV-LIFE-02: RFP0 at the memory root unwinds to Created") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out");
    return;
  }
  // The first tracked allocation of start is the root block. Later ones (the world's entity
  // pages) go through std::pmr, where out_of_memory is fatal by WO-088's design.
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  for (const bool sticky : {false, true}) {
    INFO("sticky " << sticky);
    rig g(sticky ? 222U : 221U);
    sl::result<void> started;
    {
      const rtt::scoped_arm arm(rt::failpoint::rfp0, 1, sticky);
      REQUIRE(arm.status().has_value());
      started = g.r->start();
      if (sticky) {
        // a retry under a sticky RFP0 fails the same way and still leaves nothing
        REQUIRE_FALSE(started.has_value());
        check_unwound(g, baseline, *sink);
        started = g.r->start();
      }
    }
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code == sl::errc::out_of_memory);
    check_unwound(g, baseline, *sink);
    check_retry(g, baseline);
  }
  CHECK(live_block_reports(*sink) == 0);
}

TEST_CASE("RT-INV-LIFE-02: RFP0 at every unwindable allocation visit of start unwinds") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out");
    return;
  }
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  std::uint64_t visits = 0;
  {
    rig clean(230);
    const std::uint64_t before = rt::visits(rt::failpoint::rfp0);
    REQUIRE(clean.r->start().has_value());
    visits = rt::visits(rt::failpoint::rfp0) - before;
    REQUIRE(clean.r->shutdown().has_value());
  }
  // The root block, the entity allocator's pmr page list (not swept: fatal by design), the
  // location page table. A change here means start's allocations changed: revisit the sweep.
  REQUIRE(visits == 3);
  for (const std::uint64_t k : {std::uint64_t{1}, std::uint64_t{3}}) {
    INFO("visit " << k);
    rig g(static_cast<std::uint32_t>(230U + k));
    sl::result<void> started;
    {
      const rtt::scoped_arm arm(rt::failpoint::rfp0, k);
      REQUIRE(arm.status().has_value());
      started = g.r->start();
    }
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code == sl::errc::out_of_memory);
    check_unwound(g, baseline, *sink);
    check_retry(g, baseline);
  }
  CHECK(live_block_reports(*sink) == 0);
}

TEST_CASE("RT-INV-LIFE-02: the platform check fails first and leaves nothing") {
  auto sink = std::make_shared<sl::log::memory_sink>(1024);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  rtt::scripted_platform zero(0);
  auto made = rt::runtime::create(zero.get(), rt::runtime_config{});
  REQUIRE(made.has_value());
  const auto started = (*made)->start();
  REQUIRE_FALSE(started.has_value());
  CHECK(started.error().code == sl::errc::runtime_failure);
  CHECK((*made)->state() == rt::rt_state::created);
  CHECK(settles_at(baseline));
  CHECK(live_block_reports(*sink) == 0);
}
