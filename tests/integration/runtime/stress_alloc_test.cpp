// RT-INV-STRESS-03 (SL-ARCH-003 ARCH3-R162, R042, R110, `stress_matrix` S4 and S5, `failpoints`
// RFP0): S4 allocator churn balances with zero leaks and S5 RFP0 sweeps leave the world unchanged.
// S4: 8 threads x 1e6 operations (scaled) of seeded allocate, check and release on one
// tracking_resource per thread plus a shared one for every 16th block (any-thread over new_delete),
// sizes 8 B to 64 KiB (log-uniform), alignments 1 to 64, at most 64 blocks held per thread; each
// block carries its pattern in its first and last byte. Afterwards the counters balance: allocs
// equal frees equal the counted operations, live bytes and blocks 0, no failure, no corrupt block,
// no leak record. S5: a structural batch of 2000 world_gen operations on one world; for each
// operation, RFP0 armed at its 1st, 2nd, ... allocation fails it with out_of_memory and leaves the
// world version and the oracle comparison unchanged (the world hash is compared at the first
// failure of every step-th allocating operation), until the index past its last allocation lets it
// succeed; the batch then moves on, the oracle still agrees at the end and the world leaks nothing
// when destroyed. The sweep step is 1 / SL_RUNTIME_STRESS_SCALE (every tenth index under the
// sanitizers). S5 is skipped when the failpoints are compiled out.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_alloc_test";
constexpr std::uint32_t threads = 8;
constexpr std::size_t max_held = 64;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// --- S4 ---

struct block {
  unsigned char *p = nullptr;
  std::size_t bytes = 0;
  std::size_t align = 0;
  unsigned char pattern = 0;
};

struct churn_totals {
  std::atomic<std::uint64_t> allocs{0};
  std::atomic<std::uint64_t> frees{0};
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> corrupt{0};
};

// 8 B to 64 KiB, log-uniform: 2^(3 + r % 14) plus a random part below it.
std::size_t draw_size(std::uint64_t &state) noexcept {
  const std::size_t base = std::size_t{1} << (3U + (splitmix64(state) % 13U));
  return base + static_cast<std::size_t>(splitmix64(state) % base);
}

bool intact(const block &b) noexcept {
  return b.p[0] == b.pattern && b.p[b.bytes - 1] == b.pattern;
}

// Every 16th block comes from `shared` (all threads), the others from the thread's `own`.
void churn(mem::tracking_resource &own, mem::tracking_resource &shared, std::uint64_t seed,
           std::uint64_t operations, churn_totals &out) noexcept {
  std::uint64_t state = seed;
  std::array<block, max_held> live{};
  std::array<mem::tracking_resource *, max_held> from{};
  std::size_t count = 0;
  const auto drop = [&](std::size_t i) noexcept {
    const block b = live.at(i);
    if (!intact(b)) {
      out.corrupt.fetch_add(1);
    }
    from.at(i)->release(b.p, b.bytes, b.align);
    from.at(i) = from.at(count - 1);
    out.frees.fetch_add(1);
    --count;
    live.at(i) = live.at(count);
    live.at(count) = block{};
  };
  for (std::uint64_t op = 0; op < operations; ++op) {
    if (count == 0 || (count < max_held && splitmix64(state) % 2U == 0U)) {
      block b;
      b.bytes = draw_size(state);
      b.align = std::size_t{1} << (splitmix64(state) % 7U);
      b.pattern = static_cast<unsigned char>(splitmix64(state) & 0xFFU);
      mem::tracking_resource &r = op % 16U == 0U ? shared : own;
      auto got = r.try_allocate(b.bytes, b.align, r.default_tag());
      if (!got) {
        out.errors.fetch_add(1);
        continue;
      }
      b.p = static_cast<unsigned char *>(*got);
      b.p[0] = b.pattern;
      b.p[b.bytes - 1] = b.pattern;
      live.at(count) = b;
      from.at(count) = &r;
      ++count;
      out.allocs.fetch_add(1);
    } else {
      drop(static_cast<std::size_t>(splitmix64(state) % count));
    }
  }
  while (count > 0) {
    drop(count - 1);
  }
}

// --- S5 ---

struct world_fixture {
  world_fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  // Ends the world; true when nothing leaked.
  bool destroy() {
    w.reset();
    return memory.totals().live_bytes == 0 && oracle->counter().live() == 0;
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"stress.sweep"};
  std::unique_ptr<rtt::world_oracle> oracle;
  std::unique_ptr<ecs::world> w;
};

std::uint64_t sweep_step() {
  const double scale = rtt::stress_scale();
  return scale >= 1.0 ? 1 : std::max<std::uint64_t>(1, static_cast<std::uint64_t>(1.0 / scale));
}

} // namespace

TEST_CASE("RT-INV-STRESS-03: S4 allocator churn from 8 threads balances with zero leaks") {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990004ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const std::uint64_t operations = rtt::scaled(1'000'000);
  mem::tracking_resource shared("stress.churn.shared");
  std::vector<std::unique_ptr<mem::tracking_resource>> own;
  own.reserve(threads);
  for (std::uint32_t t = 0; t < threads; ++t) {
    own.push_back(std::make_unique<mem::tracking_resource>("stress.churn.thread"));
  }
  churn_totals out;
  REQUIRE(rtt::run_watched([&] {
    std::vector<std::jthread> pool;
    pool.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
      pool.emplace_back([&own, &shared, &out, seed, operations, t] {
        churn(*own.at(t), shared, seed + (std::uint64_t{t} << 32U), operations, out);
      });
    }
  }));
  INFO(operations << " operations per thread");
  CHECK(out.errors.load() == 0);
  CHECK(out.corrupt.load() == 0);
  mem::tag_counters c = shared.totals();
  CHECK(c.allocs > 0);
  for (const auto &r : own) {
    const mem::tag_counters t = r->totals();
    c.allocs += t.allocs;
    c.frees += t.frees;
    c.live_bytes += t.live_bytes;
    c.live_blocks += t.live_blocks;
    c.failures += t.failures;
    CHECK(r->leaks().empty());
  }
  CHECK(c.allocs == out.allocs.load());
  CHECK(c.frees == out.frees.load());
  CHECK(c.allocs == c.frees);
  CHECK(c.live_bytes == 0);
  CHECK(c.live_blocks == 0);
  CHECK(c.failures == 0);
  CHECK(shared.leaks().empty());
  CHECK(out.allocs.load() >= threads * (operations / 2));
  SL_STRESS_CAP(cap);
}

TEST_CASE("RT-INV-STRESS-03: S5 RFP0 at every allocation index of a 2000-operation structural "
          "batch leaves the world unchanged") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
    return;
  }
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990005ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  constexpr std::size_t batch_ops = 2000;
  // Dry run: visits_after[i] is the cumulative RFP0 visit count after operation i.
  std::vector<rtt::world_op> ops;
  std::vector<std::uint64_t> visits_after;
  {
    world_fixture f;
    rtt::op_generator gen(seed);
    const std::uint64_t start = rt::visits(rt::failpoint::rfp0);
    bool oom = false;
    while (ops.size() < batch_ops) {
      ops.push_back(gen.next());
      const auto r = rtt::apply(ops.back(), *f.w, *f.oracle);
      oom = oom || (!r.has_value() && r.error().code == sl::errc::out_of_memory);
      visits_after.push_back(rt::visits(rt::failpoint::rfp0) - start);
    }
    CHECK_FALSE(oom);
    CHECK(f.destroy());
  }
  const std::uint64_t allocations = visits_after.back();
  REQUIRE(allocations > 20);
  // Sweep on one world: for operation i, RFP0 armed at its 1st, 2nd, ... allocation fails it with
  // out_of_memory and leaves the world unchanged, until the first index past its last allocation,
  // where the operation succeeds and the batch moves on.
  const std::uint64_t step = sweep_step();
  MESSAGE("S5: " << ops.size() << " operations make " << allocations
                 << " allocations in the dry run; sweep step " << step);
  std::uint64_t swept = 0;
  std::uint64_t changed = 0;
  std::uint64_t other_error = 0;
  std::uint64_t ok_ops = 0;
  bool leaked = false;
  std::uint64_t hashed = 0;
  {
    world_fixture f;
    std::uint64_t allocating = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      const rtt::world_op &op = ops.at(i);
      // The world hash (SHA-256 over the whole world, slow in Debug) is compared around the first
      // failure of every step-th allocating operation; the version and the oracle after every one.
      const bool allocates = visits_after.at(i) > (i == 0 ? 0 : visits_after.at(i - 1));
      const bool hash_check = allocates && (allocating++ % step) == 0;
      const sl::fabric::digest before =
          hash_check ? rtt::hash_world(*f.w, f.ids) : sl::fabric::digest{};
      const auto version = f.w->version();
      for (std::uint64_t k = 1;; k += step) {
        REQUIRE(rtt::arm(rt::failpoint::rfp0, k).has_value());
        const auto r = rtt::apply(op, *f.w, *f.oracle);
        rtt::disarm(rt::failpoint::rfp0);
        if (r.has_value()) {
          ++ok_ops;
          break;
        }
        if (r.error().code != sl::errc::out_of_memory) {
          ++other_error;
          break;
        }
        ++swept;
        if (hash_check && k == 1) {
          ++hashed;
          changed += rtt::hash_world(*f.w, f.ids) == before ? 0U : 1U;
        }
        if (f.w->version() != version || !f.oracle->compare(*f.w).empty()) {
          ++changed;
        }
        REQUIRE(k < 1'000'000);
      }
    }
    CHECK(f.oracle->compare(*f.w).empty());
    leaked = !f.destroy();
  }
  INFO("swept " << swept << " allocation indices; " << ok_ops << " operations applied; " << hashed
                << " hash comparisons");
  CHECK(swept >= allocations / (2 * step));
  CHECK(hashed > 0);
  CHECK(changed == 0);
  CHECK_FALSE(leaked);
  CHECK(ok_ops + other_error == ops.size());
  SL_STRESS_CAP(cap);
}
