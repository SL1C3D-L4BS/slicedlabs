// RT-INV-LIFE-01 (SL-ARCH-003 ARCH3-R022, ADR-0028): every (state, event) pair outside rt_states
// is invalid_state with the state unchanged, and each legal transition emits exactly one
// runtime.lifecycle event. next_state is checked over all 9 x 12 pairs against the table; a
// runtime then walks start_fail, start, run_frame, suspend, resume, fault and recovery, stop and
// stopped while a memory_sink counts the runtime.lifecycle events of its correlation scope, and
// every lifecycle call made in a state the table does not allow returns invalid_state, leaves the
// state where it was and emits nothing.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/runtime.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;
using rt::rt_event;
using rt::rt_state;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct counter {
  std::uint64_t value = 0;
};

struct control {
  std::atomic<bool> fail{false};
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

struct faulty_body {
  control *ctl = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (ctl->fail.exchange(false)) {
      ctx.fail(sl::errc::job_failed);
    }
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

struct rig {
  explicit rig(const rt::runtime_config &cfg) : plat(cfg.instance_id) {
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
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.faulty",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = faulty_body{.ctl = &ctl},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  rtt::scripted_platform plat;
  control ctl;
  std::unique_ptr<rt::runtime> r;
};

rt::runtime_config config(std::uint32_t id) {
  rt::runtime_config c;
  c.instance_id = id;
  c.workers = 2;
  c.rate = {.num = 50, .den = 1};
  return c;
}

struct row {
  rt_state from;
  rt_event event;
  rt_state to;
};

constexpr std::array<row, 15> table = {{
    {.from = rt_state::created, .event = rt_event::start, .to = rt_state::starting},
    {.from = rt_state::starting, .event = rt_event::start_ok, .to = rt_state::ready},
    {.from = rt_state::starting, .event = rt_event::start_fail, .to = rt_state::created},
    {.from = rt_state::ready, .event = rt_event::run_frame, .to = rt_state::running},
    {.from = rt_state::running, .event = rt_event::suspend, .to = rt_state::suspended},
    {.from = rt_state::suspended, .event = rt_event::resume, .to = rt_state::running},
    {.from = rt_state::ready, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::running, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::suspended, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::running, .event = rt_event::fault, .to = rt_state::faulted},
    {.from = rt_state::faulted, .event = rt_event::recover, .to = rt_state::recovering},
    {.from = rt_state::recovering, .event = rt_event::recover_ok, .to = rt_state::running},
    {.from = rt_state::recovering, .event = rt_event::recover_fail, .to = rt_state::stopping},
    {.from = rt_state::faulted, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::stopping, .event = rt_event::stopped, .to = rt_state::stopped},
}};

/// The transitions logged under `scope`, as "from>to".
std::vector<std::string> transitions(const sl::log::memory_sink &sink, std::string_view scope) {
  std::vector<std::string> out;
  for (const auto &rec : sink.records()) {
    if (rec.system == "runtime.lifecycle" && rec.scope == scope) {
      out.push_back(text(rec, "from") + ">" + text(rec, "to"));
    }
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-LIFE-01: next_state follows rt_states and refuses every other pair") {
  std::size_t legal = 0;
  for (std::size_t s = 0; s < rt::rt_state_count; ++s) {
    for (std::size_t e = 0; e < rt::rt_event_count; ++e) {
      const auto from = static_cast<rt_state>(s);
      const auto ev = static_cast<rt_event>(e);
      INFO(rt::to_string(from) << " / " << rt::to_string(ev));
      const row *expected = nullptr;
      for (const row &r : table) {
        if (r.from == from && r.event == ev) {
          expected = &r;
        }
      }
      const auto got = rt::next_state(from, ev);
      if (expected != nullptr) {
        ++legal;
        REQUIRE(got.has_value());
        CHECK(*got == expected->to);
      } else {
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error().code == sl::errc::invalid_state);
      }
    }
  }
  CHECK(legal == table.size());
  CHECK(rt::to_string(rt_state::recovering) == "Recovering");
  CHECK(rt::to_string(rt_event::recover_fail) == "recover_fail");
}

TEST_CASE("RT-INV-LIFE-01: each legal transition emits exactly one runtime.lifecycle event") {
  auto sink = std::make_shared<sl::log::memory_sink>(8192);
  const attached guard(sink);

  // start_fail: the platform check refuses instance id 0.
  {
    rtt::scripted_platform zero(0);
    auto made = rt::runtime::create(zero.get(), config(110));
    REQUIRE(made.has_value());
    auto &r = **made;
    const auto started = r.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(r.state() == rt_state::created);
    CHECK(transitions(*sink, r.scope()) ==
          std::vector<std::string>{"Created>Starting", "Starting>Created"});
  }

  rt::runtime_config c = config(111);
  c.recovery = rt::recovery_policy::restore_snapshot;
  c.snapshot_every = 1;
  rig g(c);
  REQUIRE(g.r->start().has_value());
  for (int i = 0; i < 3; ++i) {
    REQUIRE(g.frame().has_value());
  }
  g.r->request_suspend();
  REQUIRE(g.frame().has_value());
  CHECK(g.r->state() == rt_state::suspended);
  REQUIRE(g.r->resume().has_value());
  REQUIRE(g.frame().has_value());
  g.ctl.fail.store(true);
  REQUIRE(g.frame().has_value());
  CHECK(g.r->recoveries() == 1);
  CHECK(g.r->state() == rt_state::running);
  g.r->request_stop();
  REQUIRE(g.frame().has_value());
  CHECK(g.r->state() == rt_state::stopping);
  REQUIRE(g.r->shutdown().has_value());
  CHECK(g.r->state() == rt_state::stopped);
  CHECK(transitions(*sink, g.r->scope()) ==
        std::vector<std::string>{"Created>Starting", "Starting>Ready", "Ready>Running",
                                 "Running>Suspended", "Suspended>Running", "Running>Faulted",
                                 "Faulted>Recovering", "Recovering>Running", "Running>Stopping",
                                 "Stopping>Stopped"});
  for (const auto &rec : sink->records()) {
    if (rec.system == "runtime.lifecycle" && rec.scope == g.r->scope()) {
      const auto *instance = rec.find("instance");
      REQUIRE(instance != nullptr);
      CHECK(std::get<std::int64_t>(*instance) == 111);
      CHECK(rec.find("tick") != nullptr);
    }
  }
}

TEST_CASE("RT-INV-LIFE-01: lifecycle calls outside rt_states are invalid_state, state unchanged") {
  auto sink = std::make_shared<sl::log::memory_sink>(8192);
  const attached guard(sink);
  rig g(config(112));
  const std::string scope(g.r->scope());

  const auto refused = [&](sl::result<void> got, rt_state stays) {
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::invalid_state);
    CHECK(g.r->state() == stays);
  };
  refused(g.r->resume(), rt_state::created);
  refused(g.r->shutdown(), rt_state::created);
  {
    const auto ran = g.r->run_frame();
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::invalid_state);
  }
  CHECK(transitions(*sink, scope).empty());

  REQUIRE(g.r->start().has_value());
  const std::size_t after_start = transitions(*sink, scope).size();
  refused(g.r->start(), rt_state::ready);
  refused(g.r->resume(), rt_state::ready);
  CHECK(g.r->add_system(ecs::system_desc{.name = "late",
                                         .access = {},
                                         .before = {},
                                         .after = {},
                                         .fn = faulty_body{.ctl = &g.ctl},
                                         .chunked = false,
                                         .queries = {}})
            .error()
            .code == sl::errc::invalid_state);
  REQUIRE(g.frame().has_value());
  refused(g.r->resume(), rt_state::running);
  g.r->request_suspend();
  REQUIRE(g.frame().has_value());
  CHECK(g.r->state() == rt_state::suspended);
  {
    const auto ran = g.r->run_frame();
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::invalid_state);
    CHECK(g.r->state() == rt_state::suspended);
  }
  refused(g.r->start(), rt_state::suspended);
  const std::size_t before_shutdown = transitions(*sink, scope).size();
  CHECK(before_shutdown == after_start + 2); // Ready>Running, Running>Suspended
  REQUIRE(g.r->shutdown().has_value());
  refused(g.r->shutdown(), rt_state::stopped);
  refused(g.r->resume(), rt_state::stopped);
  refused(g.r->start(), rt_state::stopped);
  CHECK(transitions(*sink, scope).size() == before_shutdown + 2);
}
