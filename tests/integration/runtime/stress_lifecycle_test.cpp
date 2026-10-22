// RT-INV-STRESS-06 (SL-ARCH-003 ARCH3-R033, R022, R025, R029, R030, `stress_matrix` S8 and S9,
// `failpoints` RFP1, RFP5 and RFP9): S8 drives one runtime at a time (2 workers, a counter world,
// leak_check on) through a seeded storm of 500 lifecycle operations (scaled): start (half of them
// with RFP1 armed at a seeded spawn, which unwinds to Created with thread_spawn_failed), frames,
// RFP5 (system.fail; the frame ends Faulted under the stop policy), RFP9 (suspend.request at a
// seeded visit; Suspended at an end_frame), suspend, resume, stop and shutdown with a fresh
// runtime after it. Every observed state is reachable from the one before through the rt_states
// table (next_state, at most 4 events), every shutdown succeeds with leak_check on (no leak), and
// the storm runs under the harness watchdog (no hang). S9 runs two runtimes in one process on the
// same seed for 300 ticks (scaled), frame by frame in turn: their state hashes are equal after
// every frame, and their memory reports are independent (each names its own instance; growing
// one world leaves the other report unchanged). S8 is skipped when the failpoints are compiled
// out.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_lifecycle_test";
constexpr std::uint32_t workers = 2;
constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
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

rt::runtime_config config_for(std::uint32_t instance, std::uint64_t seed) {
  rt::runtime_config c;
  c.instance_id = instance;
  c.workers = workers;
  c.rate = {.num = 50, .den = 1};
  c.seed = seed;
  c.leak_check = true;
  return c;
}

// A runtime on a scripted platform with the counter component and its step system registered.
struct rig {
  rig(std::uint32_t instance, std::uint64_t seed) : plat(instance) {
    auto made = rt::runtime::create(plat.get(), config_for(instance, seed));
    if (!made) {
      error = "create: " + made.error().message;
      return;
    }
    r = std::move(*made);
    rt::type_builder<counter> b("stress.counter");
    SL_RT_FIELD(b, counter, value, kept);
    auto t = r->types().add(b.build());
    if (!t || !r->components().add<counter>("stress.counter", *t)) {
      error = "register counter";
      return;
    }
    auto q = ecs::query<ecs::write<counter>>::make(r->components());
    if (!q) {
      error = "query";
      return;
    }
    ecs::access_set access;
    ecs::add_query_access(access, q->access());
    const ecs::component_access needed = q->access();
    if (!r->add_system(ecs::system_desc{.name = "stress.step",
                                        .ph = rt::phase::tick,
                                        .access = std::move(access),
                                        .before = {},
                                        .after = {},
                                        .fn = step_body{.q = std::move(*q)},
                                        .chunked = false,
                                        .queries = {needed}})) {
      error = "add_system";
    }
  }

  [[nodiscard]] bool populate(std::uint32_t n, std::uint64_t base) const {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (!r->world().create_with(counter{.value = base + (std::uint64_t{i} * 977U)})) {
        return false;
      }
    }
    return true;
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] sl::result<sl::fabric::digest> hash() const {
    return rt::state_hash(r->world(), r->components(), r->types(),
                          std::span<const det::snapshot_contributor *const>{});
  }

  rtt::scripted_platform plat;
  std::unique_ptr<rt::runtime> r;
  std::string error;
};

// True when `to` is `from` or follows from it through at most `depth` events of rt_states.
bool reachable(rt::rt_state from, rt::rt_state to, int depth = 4) {
  if (from == to) {
    return true;
  }
  if (depth == 0) {
    return false;
  }
  for (std::size_t e = 0; e < rt::rt_event_count; ++e) {
    const auto next = rt::next_state(from, static_cast<rt::rt_event>(e));
    if (next && reachable(*next, to, depth - 1)) {
      return true;
    }
  }
  return false;
}

enum class op : std::uint8_t { start, frames, rfp5, rfp9, suspend, resume, stop, shutdown };
constexpr std::size_t op_count = 8;

struct storm_outcome {
  std::string error;
  std::array<std::uint64_t, op_count> done{};
  std::uint64_t rfp1_unwinds = 0;
  std::uint64_t refused_starts = 0;
  std::uint64_t rfp5_faults = 0;
  std::uint64_t rfp9_suspends = 0;
  std::uint64_t bad_transitions = 0;
  std::uint64_t bad_unwinds = 0;
  std::uint64_t shutdowns = 0;
  std::uint64_t failed_shutdowns = 0;
  std::uint64_t failed_frames = 0; // faulted, suspended or stopped frames; not an error
  std::vector<std::string> notes;
};

void note(storm_outcome &out, std::string text) {
  if (out.notes.size() < 16) {
    out.notes.push_back(std::move(text));
  }
}

bool is_live(rt::rt_state s) {
  return s == rt::rt_state::ready || s == rt::rt_state::running || s == rt::rt_state::suspended;
}

void storm(std::uint64_t seed, std::uint64_t operations, storm_outcome &out) {
  std::uint64_t state = seed;
  std::uint32_t instance = 700;
  auto g = std::make_unique<rig>(instance, seed);
  for (std::uint64_t i = 0; i < operations; ++i) {
    if (!g->error.empty()) {
      out.error = g->error;
      return;
    }
    const rt::rt_state before = g->r->state();
    const std::uint64_t r = splitmix64(state);
    auto kind = static_cast<op>(r % op_count);
    if (before == rt::rt_state::created) {
      kind = op::start;
    } else if (!is_live(before)) {
      kind = op::shutdown; // Faulted, Stopping or Stopped: end this runtime
    }
    switch (kind) {
    case op::start: {
      const bool inject = before == rt::rt_state::created && ((r >> 8U) % 2U) == 0U;
      if (inject && !rtt::failing_thread_factory::arm_rfp1(1 + ((r >> 16U) % (workers + 2)))) {
        out.error = "arm RFP1";
        return;
      }
      const auto started = g->r->start();
      rtt::disarm(rt::failpoint::rfp1);
      if (before != rt::rt_state::created) {
        // start on a live runtime is refused and changes nothing
        ++out.refused_starts;
        if (started || g->r->state() != before) {
          ++out.bad_unwinds;
          note(out, "start accepted in " + std::string(rt::to_string(before)));
        }
      } else if (!started) {
        ++out.rfp1_unwinds;
        if (started.error().code != sl::errc::thread_spawn_failed ||
            g->r->state() != rt::rt_state::created) {
          ++out.bad_unwinds;
          note(out, "start failed with " + started.error().message);
        }
      } else if (!g->populate(64, r)) {
        out.error = "populate";
        return;
      }
      break;
    }
    case op::frames:
      for (std::uint64_t f = 0; f < 1 + ((r >> 8U) % 3U); ++f) {
        out.failed_frames += g->frame() ? 0U : 1U;
      }
      break;
    case op::rfp5: {
      // A frame that runs no tick (the first one, or a suspended one) visits no system.
      const rtt::scoped_arm arm(rt::failpoint::rfp5, 1);
      for (int f = 0; f < 3 && is_live(g->r->state()) && !g->r->frame_faulted(); ++f) {
        out.failed_frames += g->frame() ? 0U : 1U;
      }
      if (g->r->frame_faulted()) {
        ++out.rfp5_faults;
        if (g->r->state() != rt::rt_state::faulted && g->r->state() != rt::rt_state::stopping &&
            g->r->state() != rt::rt_state::stopped) {
          note(out, std::string("RFP5 left ") + std::string(rt::to_string(g->r->state())));
          ++out.bad_transitions;
        }
      }
      break;
    }
    case op::rfp9: {
      const rtt::scoped_arm arm(rt::failpoint::rfp9, 1 + ((r >> 8U) % 16U));
      for (int f = 0; f < 3 && g->r->state() != rt::rt_state::suspended; ++f) {
        out.failed_frames += g->frame() ? 0U : 1U;
      }
      out.rfp9_suspends += g->r->state() == rt::rt_state::suspended ? 1U : 0U;
      break;
    }
    case op::suspend:
      g->r->request_suspend();
      out.failed_frames += g->frame() ? 0U : 1U;
      break;
    case op::resume:
      out.failed_frames += g->r->resume() ? 0U : 1U;
      break;
    case op::stop:
      g->r->request_stop();
      out.failed_frames += g->frame() ? 0U : 1U;
      break;
    case op::shutdown: {
      const auto down = g->r->shutdown();
      ++out.shutdowns;
      if (!down) {
        ++out.failed_shutdowns;
        note(out, "shutdown: " + down.error().message);
      }
      g.reset();
      g = std::make_unique<rig>(++instance, seed + i);
      break;
    }
    }
    ++out.done.at(static_cast<std::size_t>(kind));
    if (g && kind != op::shutdown && !reachable(before, g->r->state())) {
      ++out.bad_transitions;
      note(out,
           std::string(rt::to_string(before)) + " -> " + std::string(rt::to_string(g->r->state())));
    }
  }
  if (g && g->error.empty()) {
    ++out.shutdowns;
    out.failed_shutdowns += g->r->shutdown().has_value() ? 0U : 1U;
  }
}

} // namespace

TEST_CASE("RT-INV-STRESS-06: S8 a lifecycle storm with RFP1, RFP5 and RFP9 never hangs, leaks or "
          "leaves the rt_states table") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
    return;
  }
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990008ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const std::uint64_t operations = rtt::scaled(500);
  storm_outcome out;
  REQUIRE(rtt::run_watched([&] { storm(seed, operations, out); }));
  INFO(operations << " operations");
  for (const std::string &n : out.notes) {
    MESSAGE(n);
  }
  REQUIRE(out.error.empty());
  CHECK(out.bad_transitions == 0);
  CHECK(out.bad_unwinds == 0);
  CHECK(out.failed_shutdowns == 0);
  CHECK(out.shutdowns > 0);
  if (operations >= 100) {
    CHECK(out.rfp1_unwinds > 0);
    CHECK(out.rfp5_faults > 0);
    CHECK(out.rfp9_suspends > 0);
    for (const std::uint64_t n : out.done) {
      CHECK(n > 0);
    }
  }
  SL_STRESS_CAP(cap);
}

TEST_CASE("RT-INV-STRESS-06: S9 two runtimes in one process hash equal with independent memory "
          "reports") {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990009ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const auto ticks = static_cast<std::uint32_t>(rtt::scaled(300));
  std::string error;
  std::uint32_t unequal = 0;
  std::uint32_t frames = 0;
  rt::mem::memory_report a_before;
  rt::mem::memory_report a_after;
  rt::mem::memory_report b_before;
  rt::mem::memory_report b_after;
  bool a_down = false;
  bool b_down = false;
  REQUIRE(rtt::run_watched([&] {
    rig a(801, seed);
    rig b(802, seed);
    if (!a.error.empty() || !b.error.empty()) {
      error = a.error + b.error;
      return;
    }
    if (!a.r->start() || !b.r->start() || !a.populate(512, seed) || !b.populate(512, seed)) {
      error = "start or populate";
      return;
    }
    for (std::uint32_t t = 0; t < ticks; ++t) {
      if (!a.frame() || !b.frame()) {
        error = "frame";
        return;
      }
      ++frames;
      const auto ha = a.hash();
      const auto hb = b.hash();
      unequal += (ha && hb && *ha == *hb) ? 0U : 1U;
    }
    a_before = a.r->memory_report();
    b_before = b.r->memory_report();
    if (!b.populate(4096, seed + 1) || !b.frame()) {
      error = "grow b";
      return;
    }
    a_after = a.r->memory_report();
    b_after = b.r->memory_report();
    a_down = a.r->shutdown().has_value();
    b_down = b.r->shutdown().has_value();
  }));
  REQUIRE(error.empty());
  CHECK(frames == ticks);
  CHECK(unequal == 0);
  CHECK(a_before.instance == 801);
  CHECK(b_before.instance == 802);
  CHECK(a_after == a_before);
  const auto live = [](const rt::mem::memory_report &r) {
    return std::accumulate(r.tags.begin(), r.tags.end(), std::uint64_t{0},
                           [](std::uint64_t n, const auto &t) { return n + t.live_bytes; });
  };
  const std::uint64_t b_live_before = live(b_before);
  const std::uint64_t b_live_after = live(b_after);
  CHECK(b_live_after > b_live_before);
  CHECK(a_down);
  CHECK(b_down);
  SL_STRESS_CAP(cap);
}
