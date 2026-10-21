// RT-INV-LIFE-03 (SL-ARCH-003 ARCH3-R027, R053, ADR-0028): shutdown joins all workers, cancels
// pending background jobs and reports zero leaks, and lifecycle calls from another thread are
// wrong_thread. A background job held behind a cpu_fence that is never signalled is cancelled; a
// finished background job's undelivered result is discarded; every runtime thread is gone from
// /proc/self/task; the shutdown runtime.mem event and the memory report show zero live bytes and
// no leak. A host block left in root_memory makes a leak_check shutdown return leak_detected.
// start, run_frame, resume, shutdown and add_system from a second thread return wrong_thread and
// leave the state unchanged.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <variant>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct counter {
  std::uint64_t value = 0;
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

struct noop_body {
  void operator()(ecs::system_context & /*ctx*/) noexcept {}
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

bool settles_at(std::size_t want) {
  for (int i = 0; i < 2000; ++i) {
    if (thread_count() == want) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

std::int64_t int_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

struct rig {
  explicit rig(std::uint32_t id, std::uint32_t workers = 3) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = workers;
    cfg.rate = {.num = 50, .den = 1};
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

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  void populate(std::uint32_t n) const {
    for (std::uint32_t i = 0; i < n; ++i) {
      REQUIRE(r->world().create_with(counter{.value = i + 1U}).has_value());
    }
  }

  rtt::scripted_platform plat;
  std::unique_ptr<rt::runtime> r;
};

} // namespace

TEST_CASE("RT-INV-LIFE-03: shutdown joins every worker, cancels pending background jobs, no leak") {
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  const std::size_t baseline = idle_thread_count();
  rig g(301);
  REQUIRE(g.r->start().has_value());
  CHECK(thread_count() >= baseline + 3);
  g.populate(200);
  for (int i = 0; i < 6; ++i) {
    REQUIRE(g.frame().has_value());
  }

  // one background job finished with an undelivered result, one held behind a fence
  const jobs::job_handle posted = g.r->jobs().submit(
      jobs::job_desc{.fn =
                         [](jobs::job_context &ctx) noexcept {
                           const std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2},
                                                                std::byte{3}, std::byte{4}};
                           [[maybe_unused]] const auto sent = ctx.post_result(7, bytes);
                         },
                     .prio = jobs::priority::background,
                     .aff = jobs::affinity::any,
                     .name = "life.posted"});
  REQUIRE(g.r->jobs().wait(posted).has_value());
  const rt::cpu_fence never(1);
  const std::array<jobs::dependency, 1> held_on{jobs::dependency::on(never)};
  auto held = g.r->jobs().try_submit(jobs::job_desc{.fn = [](jobs::job_context &) noexcept {},
                                                    .prio = jobs::priority::background,
                                                    .aff = jobs::affinity::any,
                                                    .name = "life.held"},
                                     held_on);
  REQUIRE(held.has_value());
  CHECK(g.r->jobs().status(*held).value() == jobs::job_status::pending);

  REQUIRE(g.r->shutdown().has_value());
  CHECK(g.r->state() == rt::rt_state::stopped);
  CHECK(g.r->jobs().status(*held).value() == jobs::job_status::cancelled);
  CHECK(g.r->jobs().stats().cancelled >= 1);
  CHECK(g.r->discarded_results() == 1);
  CHECK(settles_at(baseline));

  const rt::mem::memory_report report = g.r->memory_report();
  CHECK(report.leaks.empty());
  for (const auto &t : report.tags) {
    CHECK(t.live_bytes == 0);
  }
  std::size_t shutdown_events = 0;
  for (const auto &rec : sink->records()) {
    if (rec.system == "runtime.mem" && rec.message == "shutdown memory report" &&
        rec.scope == g.r->scope()) {
      ++shutdown_events;
      CHECK(int_field(rec, "live_bytes") == 0);
      CHECK(int_field(rec, "leaks") == 0);
      const auto *flag = rec.find("shutdown");
      REQUIRE(flag != nullptr);
      CHECK(std::get<bool>(*flag));
    }
    CHECK_FALSE((rec.system == "runtime.mem" && rec.level == sl::log::level::error));
  }
  CHECK(shutdown_events == 1);
}

TEST_CASE("RT-INV-LIFE-03: a host block left in root_memory is leak_detected") {
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  rig g(302, 1);
  REQUIRE(g.r->start().has_value());
  REQUIRE(g.frame().has_value());
  auto block = g.r->root_memory().try_allocate(96, 16, g.r->root_memory().default_tag());
  REQUIRE(block.has_value());
  const auto stopped = g.r->shutdown();
  REQUIRE_FALSE(stopped.has_value());
  CHECK(stopped.error().code == sl::errc::leak_detected);
  CHECK(g.r->state() == rt::rt_state::stopped);
  CHECK_FALSE(g.r->memory_report().leaks.empty());
  g.r->root_memory().release(*block, 96, 16);
}

TEST_CASE("RT-INV-LIFE-03: lifecycle calls from another thread are wrong_thread") {
  rig g(303, 2);
  const auto off_owner = [](auto call) {
    sl::errc code = sl::errc::ok;
    std::thread other([&] { code = call(); });
    other.join();
    return code;
  };
  CHECK(off_owner([&] { return g.r->start().error().code; }) == sl::errc::wrong_thread);
  // The descriptor is built on the owner thread; only the call runs on the other one.
  ecs::system_desc other_desc{.name = "life.other",
                              .access = {},
                              .before = {},
                              .after = {},
                              .fn = noop_body{},
                              .chunked = false,
                              .queries = {}};
  CHECK(off_owner([&] { return g.r->add_system(std::move(other_desc)).error().code; }) ==
        sl::errc::wrong_thread);
  CHECK(g.r->state() == rt::rt_state::created);
  REQUIRE(g.r->start().has_value());
  CHECK(off_owner([&] { return g.r->run_frame().error().code; }) == sl::errc::wrong_thread);
  CHECK(off_owner([&] { return g.r->shutdown().error().code; }) == sl::errc::wrong_thread);
  CHECK(g.r->state() == rt::rt_state::ready);
  REQUIRE(g.frame().has_value());
  g.r->request_suspend(); // any-thread
  std::thread([&] { g.r->request_stop(); }).join();
  REQUIRE(g.frame().has_value());
  // stop wins over suspend at the same end_frame
  CHECK(g.r->state() == rt::rt_state::stopping);
  CHECK(off_owner([&] { return g.r->resume().error().code; }) == sl::errc::wrong_thread);
  CHECK(off_owner([&] { return g.r->shutdown().error().code; }) == sl::errc::wrong_thread);
  CHECK(g.r->state() == rt::rt_state::stopping);
  REQUIRE(g.r->shutdown().has_value());
}
