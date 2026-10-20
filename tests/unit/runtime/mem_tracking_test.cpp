// RT-INV-MEM-02 (SL-ARCH-003 ARCH3-R045, ADR-0023): allocated minus freed equals live bytes per
// tag and overall after 1e4 seeded operations. A model in the test counts what it allocated and
// freed per tag; the tracking_resource counters must agree with it exactly, and with zero once
// every block is released. A second case runs the contract suite from four threads at once on
// one tracking_resource (any-thread over new_delete) and checks the totals return to zero.

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/allocator_contract.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::size_t tag_count = 4;
constexpr std::uint32_t operation_count = 10000;

struct live_block {
  void *p = nullptr;
  std::size_t bytes = 0;
  std::size_t align = 0;
  std::size_t tag_index = 0;
};

struct model {
  std::uint64_t allocated_bytes = 0;
  std::uint64_t freed_bytes = 0;
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
};

std::uint64_t splitmix(std::uint64_t &state) {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

} // namespace

TEST_CASE("RT-INV-MEM-02: allocated minus freed equals live bytes per tag and overall after 1e4 "
          "seeded operations") {
  mem::tag_registry registry;
  const std::vector<std::string> names = {"runtime.root", "jobs.records", "ecs.chunks", "events"};
  std::vector<mem::memory_tag> tags(tag_count);
  for (std::size_t i = 0; i < tag_count; ++i) {
    const auto t = registry.add(names[i]);
    REQUIRE(t.has_value());
    tags[i] = *t;
  }
  REQUIRE(registry.freeze().has_value());
  mem::tracking_resource r("tracking", tags[0], &registry);

  std::vector<model> expected(tag_count);
  std::vector<live_block> live;
  std::uint64_t state = 0x083;
  for (std::uint32_t op = 0; op < operation_count; ++op) {
    if (live.empty() || splitmix(state) % 5U < 3U) {
      live_block b{.p = nullptr,
                   .bytes = static_cast<std::size_t>(splitmix(state) % 700U),
                   .align = std::size_t{1} << (splitmix(state) % 13U),
                   .tag_index = static_cast<std::size_t>(splitmix(state) % tag_count)};
      auto got = r.try_allocate(b.bytes, b.align, tags[b.tag_index]);
      REQUIRE(got.has_value());
      b.p = *got;
      live.push_back(b);
      expected[b.tag_index].allocated_bytes += mem::effective_size(b.bytes);
      ++expected[b.tag_index].allocs;
    } else {
      const auto i = static_cast<std::size_t>(splitmix(state) % live.size());
      const live_block b = live[i];
      live[i] = live.back();
      live.pop_back();
      r.release(b.p, b.bytes, b.align);
      expected[b.tag_index].freed_bytes += mem::effective_size(b.bytes);
      ++expected[b.tag_index].frees;
    }
  }

  mem::tag_counters sum;
  for (std::size_t i = 0; i < tag_count; ++i) {
    const mem::tag_counters c = r.counters(tags[i]);
    CHECK(c.live_bytes == expected[i].allocated_bytes - expected[i].freed_bytes);
    CHECK(c.live_blocks == expected[i].allocs - expected[i].frees);
    CHECK(c.allocs == expected[i].allocs);
    CHECK(c.frees == expected[i].frees);
    CHECK(c.failures == 0);
    CHECK(c.peak_bytes >= c.live_bytes);
    sum.live_bytes += c.live_bytes;
    sum.live_blocks += c.live_blocks;
    sum.allocs += c.allocs;
    sum.frees += c.frees;
  }
  const mem::tag_counters total = r.totals();
  CHECK(total.live_bytes == sum.live_bytes);
  CHECK(total.live_blocks == sum.live_blocks);
  CHECK(total.live_blocks == live.size());
  CHECK(total.allocs == sum.allocs);
  CHECK(total.frees == sum.frees);
  CHECK(total.peak_bytes >= total.live_bytes);
  CHECK(r.counters(mem::memory_tag{}).allocs == 0);

  for (const live_block &b : live) {
    r.release(b.p, b.bytes, b.align);
  }
  for (std::size_t i = 0; i < tag_count; ++i) {
    CHECK(r.counters(tags[i]).live_bytes == 0);
    CHECK(r.counters(tags[i]).live_blocks == 0);
    CHECK(r.counters(tags[i]).peak_bytes > 0);
  }
  CHECK(r.totals().live_bytes == 0);
  CHECK(r.totals().allocs == r.totals().frees);
  CHECK(r.leaks().empty());
}

TEST_CASE("RT-INV-MEM-02: the counters return to zero after four threads run the contract suite "
          "at once") {
  constexpr std::size_t thread_count = 4;
  mem::tracking_resource r("shared");
  std::vector<std::vector<std::string>> failures(thread_count);
  {
    std::vector<std::jthread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
      threads.emplace_back([&r, &failures, t] {
        failures[t] = rtt::run_allocator_contract(
            r, {.seed = t + 1, .operations = 1500, .owner_thread_only = false});
      });
    }
  }
  for (const auto &f : failures) {
    CHECK(f.empty());
  }
  const mem::tag_counters total = r.totals();
  CHECK(total.live_bytes == 0);
  CHECK(total.live_blocks == 0);
  CHECK(total.allocs == total.frees);
  CHECK(total.allocs > 0);
  CHECK(total.peak_bytes > 0);
}
