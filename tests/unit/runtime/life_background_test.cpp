// RT-INV-LIFE-08 (SL-ARCH-003 ARCH3-R030, ADR-0028): a restore while background jobs are in flight
// discards their results and the continued run equals the original. A ledger entity counts its
// ticks (seq); on every odd seq a pre_tick system submits a background job that posts a value
// derived from seq; the apply_inputs system folds the results the frame scheduler drained at
// begin_frame into the ledger. Snapshots every 2 ticks land where no job is in flight; a fault at
// an odd tick leaves the job submitted in the faulted frame with an undelivered result. The
// restore discards it (discarded_results), and from the restored tick on the run hashes equal to
// a fault-free run after as many applied ticks, under both queue kinds.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t result_kind = 9;
constexpr rt::time::tick end_tick = 30;

struct ledger {
  std::uint64_t seq = 0;
  std::uint64_t sum = 0;
};

struct shared {
  rt::runtime *runtime = nullptr;
  std::atomic<bool> fail{false};
  std::atomic<std::uint64_t> submitted{0};
};

struct post_job {
  std::uint64_t value = 0;
  void operator()(jobs::job_context &ctx) const noexcept {
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    [[maybe_unused]] const auto posted = ctx.post_result(result_kind, bytes);
  }
};

struct submit_body {
  ecs::query<ecs::write<ledger>> q;
  shared *s = nullptr;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [&ctx, this](ecs::entity /*e*/, ledger &l) noexcept {
      l.seq += 1;
      if (l.seq % 2 == 1) {
        static_cast<void>(ctx.jobs().submit(jobs::job_desc{.fn = post_job{.value = (l.seq * 7) + 3},
                                                           .prio = jobs::priority::background,
                                                           .aff = jobs::affinity::any,
                                                           .name = "life.background"}));
        s->submitted.fetch_add(1);
      }
    });
  }
};

struct apply_body {
  ecs::query<ecs::write<ledger>> q;
  shared *s = nullptr;
  void operator()(ecs::system_context &ctx) noexcept {
    const auto inputs = s->runtime->frames().frame_inputs();
    ctx.for_each(q, [inputs](ecs::entity /*e*/, ledger &l) noexcept {
      for (const det::external_input &in : inputs) {
        if (in.kind == result_kind && in.bytes.size() == sizeof(std::uint64_t)) {
          std::uint64_t v = 0;
          std::memcpy(&v, in.bytes.data(), sizeof(v));
          l.sum = (l.sum * 1000003U) + v;
        }
      }
    });
  }
};

struct faulty_body {
  shared *s = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (s->fail.exchange(false)) {
      ctx.fail(sl::errc::job_failed);
    }
  }
};

ecs::system_desc ledger_system(std::string_view name, rt::phase ph, ecs::system_fn fn,
                               const ecs::component_access &needed) {
  ecs::access_set access;
  ecs::add_query_access(access, needed);
  return ecs::system_desc{.name = name,
                          .ph = ph,
                          .access = std::move(access),
                          .before = {},
                          .after = {},
                          .fn = std::move(fn),
                          .chunked = false,
                          .queries = {needed}};
}

struct rig {
  rig(std::uint32_t id, rt::queue_kind queue) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = 3;
    cfg.rate = {.num = 50, .den = 1};
    cfg.queue = queue;
    cfg.recovery = rt::recovery_policy::restore_snapshot;
    cfg.snapshot_every = 2;
    auto made = rt::runtime::create(plat.get(), cfg);
    REQUIRE(made.has_value());
    r = std::move(*made);
    s.runtime = r.get();
    rt::type_builder<ledger> b("life.ledger");
    SL_RT_FIELD(b, ledger, seq, kept);
    SL_RT_FIELD(b, ledger, sum, kept);
    auto t = r->types().add(b.build());
    REQUIRE(t.has_value());
    REQUIRE(r->components().add<ledger>("life.ledger", *t).has_value());
    auto q1 = ecs::query<ecs::write<ledger>>::make(r->components());
    auto q2 = ecs::query<ecs::write<ledger>>::make(r->components());
    REQUIRE(q1.has_value());
    REQUIRE(q2.has_value());
    const ecs::component_access needed = q1->access();
    REQUIRE(r->add_system(ledger_system("life.submit", rt::phase::pre_tick,
                                        submit_body{.q = std::move(*q1), .s = &s}, needed))
                .has_value());
    REQUIRE(r->add_system(ledger_system("life.apply", rt::phase::apply_inputs,
                                        apply_body{.q = std::move(*q2), .s = &s}, needed))
                .has_value());
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.faulty",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = faulty_body{.s = &s},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
    REQUIRE(r->start().has_value());
    REQUIRE(r->world().create_with(ledger{}).has_value());
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }

  [[nodiscard]] sl::fabric::digest hash() const {
    auto h = rt::state_hash(r->world(), r->components(), r->types(),
                            std::span<const det::snapshot_contributor *const>{});
    REQUIRE(h.has_value());
    return *h;
  }

  rtt::scripted_platform plat;
  shared s;
  std::unique_ptr<rt::runtime> r;
};

} // namespace

TEST_CASE("RT-INV-LIFE-08: a restore with a background job in flight discards its result") {
  for (const rt::queue_kind queue : {rt::queue_kind::work_stealing, rt::queue_kind::locked}) {
    INFO("queue " << static_cast<int>(queue));
    std::map<rt::time::tick, sl::fabric::digest> original;
    {
      rig g(801, queue);
      while (g.now() < end_tick + 4) {
        const auto ran = g.frame();
        REQUIRE(ran.has_value());
        original[g.now()] = g.hash();
      }
      CHECK(g.s.submitted.load() >= end_tick / 2);
      CHECK(g.r->discarded_results() == 0);
      REQUIRE(g.r->shutdown().has_value());
    }

    rig g(802, queue);
    while (g.now() < 8) {
      REQUIRE(g.frame().has_value());
    }
    // the next frame runs tick 9 (odd): its job's result is undelivered at the fault check
    const std::uint64_t submitted = g.s.submitted.load();
    g.s.fail.store(true);
    REQUIRE(g.frame().has_value());
    REQUIRE(g.now() == 9);
    CHECK(g.s.submitted.load() == submitted + 1);
    CHECK(g.r->recoveries() == 1);
    CHECK(g.r->discarded_results() == 1);
    REQUIRE(g.r->last_fault().has_value());
    const rt::fault_report fault = g.r->last_fault().value_or(rt::fault_report{});
    REQUIRE(fault.restored_tick.has_value());
    rt::time::tick applied = fault.restored_tick.value_or(0);
    CHECK(applied == 8);
    CHECK(g.hash() == original.at(applied));
    while (applied < end_tick) {
      const auto ran = g.frame();
      REQUIRE(ran.has_value());
      applied += ran->ticks_run;
      INFO("applied " << applied);
      REQUIRE(original.contains(applied));
      CHECK(g.hash() == original.at(applied));
    }
    CHECK(g.r->discarded_results() == 1);
    REQUIRE(g.r->shutdown().has_value());
  }
}
