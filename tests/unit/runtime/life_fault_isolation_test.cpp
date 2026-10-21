// RT-INV-LIFE-05 (SL-ARCH-003 ARCH3-R028, ADR-0028): the faulted frame's command buffers, events
// and observations are never applied and one sl-runtime-fault/1 report is produced. Two spawner
// systems record a spawn command every tick (tick and post_tick); a system fails in the tick
// phase. Under policy stop the faulted frame adds no entity, run_frame returns runtime_faulted,
// no further frame runs and frame_faulted() was set in the observation_flush slot (the link skips
// that flush). Under restore_snapshot the world and the event lists (events_contributor) after the
// faulted frame hash equal to a fault-free run at the restored tick, so neither the frame's
// commands nor its events survive. Each fault gives exactly one runtime.fault event, last_fault
// carries the same report, and to_json reproduces schemas/runtime/examples/fault.json byte for
// byte. fail_fatal (job_fatal), RFP5 (system.fail) and RFP3 (job.fatal) give the same isolation.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct counter {
  std::uint64_t value = 0;
};

struct ping {
  std::uint64_t value = 0;
};

struct control {
  std::atomic<bool> fail{false};
  std::atomic<bool> fatal{false};
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

struct spawn_body {
  void operator()(ecs::system_context &ctx) noexcept {
    static_cast<void>(ctx.commands().spawn_with(counter{.value = 5}));
  }
};

struct faulty_body {
  control *ctl = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (ctl->fail.exchange(false)) {
      ctx.fail(sl::errc::runtime_failure);
    }
    if (ctl->fatal.exchange(false)) {
      ctx.job.fail_fatal(sl::errc::job_failed);
    }
  }
};

struct writer_body {
  rt::event_channel<ping> *channel = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    const rt::writer_key w{.tick = ctx.tick(),
                           .ph = rt::phase::tick,
                           .system_index = ctx.system_index(),
                           .chunk_index = 0};
    [[maybe_unused]] const auto written = channel->write(w, ping{.value = 3});
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

std::string text(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v)
                                                                 : std::string("<none>");
}

ecs::system_desc structural(std::string_view name, rt::phase ph, ecs::system_fn fn) {
  ecs::access_set access;
  access.structural = true;
  return ecs::system_desc{.name = name,
                          .ph = ph,
                          .access = std::move(access),
                          .before = {},
                          .after = {},
                          .fn = std::move(fn),
                          .chunked = false,
                          .queries = {}};
}

struct rig {
  rig(std::uint32_t id, rt::recovery_policy policy) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = 2;
    cfg.rate = {.num = 50, .den = 1};
    cfg.recovery = policy;
    cfg.snapshot_every = 1;
    auto made = rt::runtime::create(plat.get(), cfg);
    REQUIRE(made.has_value());
    r = std::move(*made);
    rt::type_builder<counter> b("life.counter");
    SL_RT_FIELD(b, counter, value, kept);
    auto t = r->types().add(b.build());
    REQUIRE(t.has_value());
    REQUIRE(r->components().add<counter>("life.counter", *t).has_value());
    rt::type_builder<ping> p("life.ping");
    SL_RT_FIELD(p, ping, value, kept);
    REQUIRE(r->events().add<ping>("life.ping", p.build()).has_value());
    REQUIRE(r->events().freeze().has_value());
    auto ch = rt::event_channel<ping>::make(r->events());
    REQUIRE(ch.has_value());
    channel = std::make_unique<rt::event_channel<ping>>(std::move(*ch));
    const std::array<rt::event_stream *, 1> streams{&channel->stream()};
    events = std::make_unique<rt::events_contributor>(streams);
    r->add_contributor(*events);

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
    REQUIRE(r->add_system(structural("life.spawn", rt::phase::tick, spawn_body{})).has_value());
    REQUIRE(r->add_system(structural("life.late_spawn", rt::phase::post_tick, spawn_body{}))
                .has_value());
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.writer",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = writer_body{.channel = channel.get()},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.faulty",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = faulty_body{.ctl = &ctl},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
    REQUIRE(r->start().has_value());
    rt::attach(channel->stream(), r->frames());
    r->frames().add_boundary_hook(rt::boundary_slot::observation_flush,
                                  [this](const rt::boundary_context & /*bc*/) noexcept {
                                    observed.push_back(r->frame_faulted());
                                  });
    for (std::uint32_t i = 0; i < 32; ++i) {
      REQUIRE(r->world().create_with(counter{.value = i + 1U}).has_value());
    }
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }
  [[nodiscard]] std::uint32_t alive() const { return r->world().entities().alive(); }

  [[nodiscard]] sl::fabric::digest hash() const {
    const std::array<const det::snapshot_contributor *, 1> contributors{events.get()};
    auto h = rt::state_hash(r->world(), r->components(), r->types(), contributors);
    REQUIRE(h.has_value());
    return *h;
  }

  /// The runtime.fault events of this runtime.
  [[nodiscard]] std::vector<sl::log::memory_sink::record>
  faults(const sl::log::memory_sink &sink) const {
    std::vector<sl::log::memory_sink::record> out;
    for (auto &rec : sink.records()) {
      if (rec.system == "runtime.fault" && rec.scope == r->scope()) {
        out.push_back(std::move(rec));
      }
    }
    return out;
  }

  rtt::scripted_platform plat;
  control ctl;
  std::unique_ptr<rt::event_channel<ping>> channel;
  std::unique_ptr<rt::events_contributor> events;
  std::vector<bool> observed;
  std::unique_ptr<rt::runtime> r;
};

/// A fault-free run's hash after every frame, keyed by the logical tick.
std::map<rt::time::tick, sl::fabric::digest> reference(std::uint32_t frames) {
  rig g(500, rt::recovery_policy::restore_snapshot);
  std::map<rt::time::tick, sl::fabric::digest> out;
  for (std::uint32_t i = 0; i < frames; ++i) {
    REQUIRE(g.frame().has_value());
    out[g.now()] = g.hash();
  }
  REQUIRE(g.r->shutdown().has_value());
  return out;
}

/// The one report of a fault: one runtime.fault event (level error) whose fields and report match
/// last_fault.
void check_one_report(rig &g, const sl::log::memory_sink &sink, std::string_view kind) {
  const auto found = g.faults(sink);
  REQUIRE(found.size() == 1);
  REQUIRE(g.r->last_fault().has_value());
  const rt::fault_report last = g.r->last_fault().value_or(rt::fault_report{});
  CHECK(last.kind == kind);
  CHECK(found[0].level == sl::log::level::error);
  CHECK(text(found[0], "kind") == kind);
  CHECK(text(found[0], "action") == last.action);
  CHECK(text(found[0], "system") == last.system);
  CHECK(text(found[0], "report") == rt::to_json(last));
  CHECK(last.instance == g.r->config().instance_id);
}

} // namespace

TEST_CASE("RT-INV-LIFE-05: to_json reproduces the sl-runtime-fault/1 example") {
  rt::fault_report report;
  report.instance = 1;
  report.frame = 451;
  report.tick = 450;
  report.kind = "system_fail";
  report.code = sl::errc::runtime_failure;
  report.system = "lantern_flicker";
  report.job = "lantern_flicker#3";
  report.action = "restore";
  report.restored_tick = 300;
  std::ifstream in(std::string(SL_RUNTIME_SOURCE_DIR) + "/schemas/runtime/examples/fault.json",
                   std::ios::binary);
  REQUIRE(in.good());
  const std::string expected((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  CHECK(rt::to_json(report) == expected);
  report.restored_tick.reset();
  report.system = "a \"quoted\" name";
  const std::string none = rt::to_json(report);
  CHECK(none.find("\"restored_tick\": null\n}\n") != std::string::npos);
  CHECK(none.find(R"("system": "a \"quoted\" name")") != std::string::npos);
}

TEST_CASE("RT-INV-LIFE-05: under policy stop the faulted frame applies no command") {
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  rig g(501, rt::recovery_policy::stop);
  for (int i = 0; i < 5; ++i) {
    REQUIRE(g.frame().has_value());
  }
  const std::uint32_t before = g.alive();
  CHECK(before > 32); // the spawners ran
  const std::size_t observed_before = g.observed.size();
  g.ctl.fail.store(true);
  const auto ran = g.frame();
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == sl::errc::runtime_faulted);
  CHECK(g.alive() == before);
  CHECK(g.r->frame_faulted());
  CHECK(g.r->state() == rt::rt_state::stopping);
  REQUIRE(g.observed.size() == observed_before + 1);
  CHECK(g.observed.back());
  for (std::size_t i = 0; i < observed_before; ++i) {
    CHECK_FALSE(g.observed[i]);
  }
  check_one_report(g, *sink, "system_fail");
  REQUIRE(g.r->last_fault().has_value());
  const rt::fault_report last = g.r->last_fault().value_or(rt::fault_report{});
  CHECK(last.system == "life.faulty");
  CHECK(last.job.starts_with("life.faulty#"));
  CHECK(last.code == sl::errc::runtime_failure);
  CHECK(last.action == "stop");
  CHECK_FALSE(last.restored_tick.has_value());
  CHECK(last.tick == g.now());
  // no further frame: nothing of the faulted frame is ever read
  const auto next = g.frame();
  REQUIRE_FALSE(next.has_value());
  CHECK(next.error().code == sl::errc::invalid_state);
  CHECK(g.alive() == before);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-05: under restore the faulted frame's commands and events are undone") {
  const auto ref = reference(12);
  auto sink = std::make_shared<sl::log::memory_sink>(4096);
  const attached guard(sink);
  rig g(502, rt::recovery_policy::restore_snapshot);
  for (int i = 0; i < 8; ++i) {
    REQUIRE(g.frame().has_value());
    REQUIRE(ref.contains(g.now()));
    CHECK(g.hash() == ref.at(g.now()));
  }
  g.ctl.fail.store(true);
  REQUIRE(g.frame().has_value());
  CHECK(g.r->frame_faulted());
  CHECK(g.r->state() == rt::rt_state::running);
  check_one_report(g, *sink, "system_fail");
  REQUIRE(g.r->last_fault().has_value());
  const rt::fault_report last = g.r->last_fault().value_or(rt::fault_report{});
  CHECK(last.action == "restore");
  REQUIRE(last.restored_tick.has_value());
  CHECK(last.restored_tick.value_or(0) == g.now() - 1);
  REQUIRE(ref.contains(last.restored_tick.value_or(0)));
  CHECK(g.hash() == ref.at(last.restored_tick.value_or(0)));
  CHECK(g.observed.back());
  REQUIRE(g.frame().has_value());
  CHECK_FALSE(g.r->frame_faulted());
  CHECK_FALSE(g.observed.back());
  CHECK(g.faults(*sink).size() == 1);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-05: fail_fatal, RFP5 and RFP3 faults are isolated the same way") {
  struct trigger {
    const char *name = nullptr;
    rt::failpoint point{};
    bool fatal = false;
    const char *kind = nullptr;
  };
  const std::array<trigger, 3> triggers{{
      {.name = "fail_fatal", .point = rt::failpoint::rfp3, .fatal = true, .kind = "job_fatal"},
      {.name = "RFP5", .point = rt::failpoint::rfp5, .fatal = false, .kind = "system_fail"},
      {.name = "RFP3", .point = rt::failpoint::rfp3, .fatal = false, .kind = "job_fatal"},
  }};
  std::uint32_t id = 510;
  for (const trigger &t : triggers) {
    INFO(t.name);
    const bool by_failpoint = std::string_view(t.name) != "fail_fatal";
    if (by_failpoint && !rt::faults_compiled) {
      continue;
    }
    auto sink = std::make_shared<sl::log::memory_sink>(4096);
    const attached guard(sink);
    rig g(++id, rt::recovery_policy::stop);
    for (int i = 0; i < 4; ++i) {
      REQUIRE(g.frame().has_value());
    }
    const std::uint32_t before = g.alive();
    sl::result<rt::frame_stats> ran;
    if (by_failpoint) {
      const rtt::scoped_arm arm(t.point, 1);
      REQUIRE(arm.status().has_value());
      ran = g.frame();
    } else {
      g.ctl.fatal.store(t.fatal);
      ran = g.frame();
    }
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::runtime_faulted);
    CHECK(g.alive() == before);
    CHECK(g.r->frame_faulted());
    check_one_report(g, *sink, t.kind);
    REQUIRE(g.r->shutdown().has_value());
  }
}
