// RT-INV-ALLOC-05 (SL-ARCH-003 ARCH3-R049, ADR-0024): synchronized_resource under 8 threads of
// seeded churn balances its tracking and is TSan clean. Three workloads: a pool behind the mutex
// (the pool is owner-thread on its own), a tracking_resource behind it with exact per-tag counts,
// and the allocator contract run from 8 threads at once over a synchronized arena and stack.
// Operations scale with SL_RUNTIME_STRESS_SCALE (0.1 in sanitizer builds).

#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/allocator_contract.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::uint32_t threads = 8;
constexpr std::uint64_t base_seed = 0x5EED0840005ULL;

class stream {
public:
  explicit stream(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

private:
  std::uint64_t state_;
};

double stress_scale() {
  const char *text = std::getenv("SL_RUNTIME_STRESS_SCALE");
  if (text == nullptr) {
    return 1.0;
  }
  char *end = nullptr;
  const double v = std::strtod(text, &end);
  return end != text && v > 0.0 ? v : 1.0;
}

std::uint32_t scaled(std::uint32_t full) {
  const double n = static_cast<double>(full) * stress_scale();
  return n < 1000.0 ? 1000U : static_cast<std::uint32_t>(n);
}

struct held {
  void *p = nullptr;
  std::size_t bytes = 0;
  std::size_t align = 0;
  unsigned char pattern = 0;
};

struct churn_result {
  std::atomic<std::uint64_t> allocs{0};
  std::atomic<std::uint64_t> frees{0};
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> corrupt{0};
};

bool intact(const held &h) {
  const auto *b = static_cast<const unsigned char *>(h.p);
  for (std::size_t i = 0; i < mem::effective_size(h.bytes); ++i) {
    if (b[i] != h.pattern) {
      return false;
    }
  }
  return true;
}

// One thread's churn: seeded allocate, verify and release with at most 64 blocks held.
void churn(mem::resource &r, std::uint64_t seed, std::uint32_t operations, std::size_t max_bytes,
           std::uint32_t max_align_log2, churn_result &out) {
  stream rng(seed);
  std::vector<held> live;
  live.reserve(64);
  for (std::uint32_t op = 0; op < operations; ++op) {
    if (live.empty() || (live.size() < 64 && rng.next() % 2U == 0U)) {
      held h{.p = nullptr,
             .bytes = static_cast<std::size_t>(rng.next() % (max_bytes + 1)),
             .align = std::size_t{1} << (rng.next() % (max_align_log2 + 1)),
             .pattern = static_cast<unsigned char>(rng.next() & 0xFFU)};
      auto got = r.try_allocate(h.bytes, h.align, r.default_tag());
      if (!got.has_value()) {
        out.errors.fetch_add(1);
        continue;
      }
      h.p = *got;
      std::memset(h.p, h.pattern, mem::effective_size(h.bytes));
      live.push_back(h);
      out.allocs.fetch_add(1);
    } else {
      const auto i = static_cast<std::size_t>(rng.next() % live.size());
      if (!intact(live[i])) {
        out.corrupt.fetch_add(1);
      }
      r.release(live[i].p, live[i].bytes, live[i].align);
      live[i] = live.back();
      live.pop_back();
      out.frees.fetch_add(1);
    }
  }
  for (const held &h : live) {
    if (!intact(h)) {
      out.corrupt.fetch_add(1);
    }
    r.release(h.p, h.bytes, h.align);
    out.frees.fetch_add(1);
  }
}

void run_threads(mem::resource &r, std::uint32_t operations, std::size_t max_bytes,
                 std::uint32_t max_align_log2, churn_result &out) {
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (std::uint32_t t = 0; t < threads; ++t) {
    pool.emplace_back([&r, t, operations, max_bytes, max_align_log2, &out] {
      churn(r, base_seed + (std::uint64_t{t} << 8U), operations, max_bytes, max_align_log2, out);
    });
  }
  for (std::thread &t : pool) {
    t.join();
  }
}

} // namespace

TEST_CASE("RT-INV-ALLOC-05: synchronized_resource over a pool balances under 8 threads of churn") {
  const std::uint32_t operations = scaled(20000);
  mem::tracking_resource upstream("upstream");
  churn_result out;
  {
    mem::pool_resource pool(upstream, 256, 16);
    mem::synchronized_resource shared(pool);
    run_threads(shared, operations, 256, 4, out);
    CHECK(out.errors.load() == 0);
    CHECK(out.corrupt.load() == 0);
    CHECK(out.allocs.load() == out.frees.load());
    CHECK(pool.free_blocks() == pool.total_blocks());
    CHECK(pool.check_free_list());
    CHECK(upstream.totals().live_bytes == pool.slabs() * mem::pool_slab_bytes);
  }
  const mem::tag_counters t = upstream.totals();
  CHECK(t.live_bytes == 0);
  CHECK(t.allocs == t.frees);
}

TEST_CASE("RT-INV-ALLOC-05: synchronized_resource over a tracking_resource counts every block") {
  const std::uint32_t operations = scaled(20000);
  mem::tag_registry registry;
  const auto tag = registry.add("events");
  REQUIRE(tag.has_value());
  mem::tracking_resource tracked("tracked", *tag, &registry);
  mem::synchronized_resource shared(tracked);
  CHECK(shared.default_tag() == *tag);
  churn_result out;
  run_threads(shared, operations, 1024, 6, out);
  CHECK(out.errors.load() == 0);
  CHECK(out.corrupt.load() == 0);
  const mem::tag_counters c = tracked.counters(*tag);
  CHECK(c.live_bytes == 0);
  CHECK(c.live_blocks == 0);
  CHECK(c.allocs == out.allocs.load());
  CHECK(c.frees == out.frees.load());
  CHECK(c.allocs == c.frees);
}

TEST_CASE("RT-INV-ALLOC-05: the allocator contract runs from 8 threads over synchronized "
          "resources") {
  const std::uint32_t operations = scaled(2000);
  mem::tracking_resource upstream("upstream");
  {
    mem::arena_resource arena(upstream);
    mem::stack_resource stack(upstream, std::size_t{32} << 20U);
    mem::synchronized_resource over_arena(arena);
    mem::synchronized_resource over_stack(stack);
    std::atomic<std::uint64_t> failures{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        mem::resource &r = t % 2U == 0U ? static_cast<mem::resource &>(over_arena) : over_stack;
        const std::vector<std::string> f = rtt::run_allocator_contract(
            r, rtt::contract_options{
                   .seed = base_seed + t, .operations = operations, .owner_thread_only = false});
        failures.fetch_add(f.size());
      });
    }
    for (std::thread &t : pool) {
      t.join();
    }
    CHECK(failures.load() == 0);
    CHECK(stack.used() == 0);
  }
  CHECK(upstream.totals().live_bytes == 0);
}
