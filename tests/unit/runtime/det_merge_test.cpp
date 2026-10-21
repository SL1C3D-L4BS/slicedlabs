// RT-INV-DET-03 (SL-ARCH-003 ARCH3-R086, `determinism` ordered_key and merge_points): over 200
// seeded runs, the same keyed items split across 1, 2, 4 and 8 producer buffers (each filled by
// its own thread, one writer per buffer, keys increasing per buffer) merge through merge_ordered
// into the same sequence, equal to the items sorted by ordered_key, whatever the buffer order.
// Keys compare lexicographically over (tick, phase, system_index, chunk_index, local_seq); a
// non-increasing push and a key repeated across buffers are Debug assertions.

#include "sl/core/assert.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace det = sl::runtime::det;

struct splitmix64 {
  std::uint64_t state = 0;
  std::uint64_t next() noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
};

struct item {
  det::ordered_key key;
  std::uint64_t payload = 0;
};

// Unique keys for one run, in strictly increasing key order by construction: a seeded subset of
// (tick, phase, system_index, chunk_index) with one to three entries each (local_seq counts up).
std::vector<item> make_items(std::uint64_t seed) {
  splitmix64 rng{seed};
  std::vector<item> items;
  std::uint32_t seq = 0;
  for (std::uint64_t tick = 100; tick < 103; ++tick) {
    for (std::uint8_t phase = 0; phase < 5; ++phase) {
      for (std::uint16_t system = 0; system < 4; ++system) {
        for (std::uint32_t chunk = 0; chunk < 8; ++chunk) {
          if (rng.next() % 3U != 0U) {
            continue;
          }
          const std::uint64_t count = 1 + (rng.next() % 3U);
          for (std::uint64_t c = 0; c < count; ++c) {
            const det::ordered_key k{.tick = tick,
                                     .phase = phase,
                                     .system_index = system,
                                     .chunk_index = chunk,
                                     .local_seq = seq++};
            items.push_back(item{.key = k, .payload = rng.next()});
          }
        }
      }
    }
  }
  return items;
}

// Splits the items over `producers` buffers by chunk (a job per chunk set); each producer thread
// pushes its items in key order (a filter of increasing items stays increasing); the buffers are
// handed to the merge in a seeded order.
std::vector<item> run(const std::vector<item> &items, std::size_t producers, std::uint64_t seed) {
  std::vector<std::vector<item>> work(producers);
  for (const item &it : items) {
    work[(it.key.chunk_index + it.key.system_index) % producers].push_back(it);
  }
  std::vector<det::ordered_buffer<std::uint64_t>> buffers(producers);
  const auto fill = [&](std::size_t p) noexcept {
    for (const item &it : work[p]) {
      buffers[p].push(it.key, it.payload);
    }
  };
  if (producers == 1) {
    fill(0);
  } else {
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (std::size_t p = 0; p < producers; ++p) {
      threads.emplace_back(fill, p);
    }
    for (std::thread &t : threads) {
      t.join();
    }
  }
  std::vector<det::ordered_buffer<std::uint64_t> *> order;
  order.reserve(producers);
  std::ranges::transform(buffers, std::back_inserter(order),
                         [](det::ordered_buffer<std::uint64_t> &b) noexcept { return &b; });
  splitmix64 shuffle{seed};
  for (std::size_t i = order.size(); i > 1; --i) {
    std::swap(order[i - 1], order[shuffle.next() % i]);
  }
  std::vector<item> merged;
  det::merge_ordered(std::span<det::ordered_buffer<std::uint64_t> *const>(order),
                     [&](const det::ordered_key &k, std::uint64_t &v) {
                       merged.push_back(item{.key = k, .payload = v});
                     });
  return merged;
}

bool same(const std::vector<item> &a, const std::vector<item> &b) {
  return std::ranges::equal(
      a, b, [](const item &x, const item &y) { return x.key == y.key && x.payload == y.payload; });
}

} // namespace

TEST_CASE("RT-INV-DET-03: merge_ordered yields one sequence for 1, 2, 4 and 8 producers") {
  for (std::uint64_t run_index = 0; run_index < 200; ++run_index) {
    CAPTURE(run_index);
    const std::vector<item> items = make_items(0xD3E7'0000ULL + run_index);
    std::vector<item> sorted = items;
    std::ranges::sort(sorted, [](const item &a, const item &b) { return a.key < b.key; });
    const std::vector<item> one = run(items, 1, run_index);
    REQUIRE(same(one, sorted));
    for (const std::size_t producers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
      CAPTURE(producers);
      CHECK(same(run(items, producers, (run_index * 31) + producers), one));
    }
  }
}

TEST_CASE("RT-INV-DET-03: ordered_key compares lexicographically, field by field") {
  const det::ordered_key base{
      .tick = 5, .phase = 2, .system_index = 3, .chunk_index = 4, .local_seq = 9};
  det::ordered_key later_tick = base;
  later_tick.tick = 6;
  later_tick.phase = 0;
  later_tick.local_seq = 0;
  CHECK(base < later_tick);
  det::ordered_key later_phase = base;
  later_phase.phase = 3;
  later_phase.system_index = 0;
  CHECK(base < later_phase);
  det::ordered_key later_system = base;
  later_system.system_index = 4;
  later_system.chunk_index = 0;
  CHECK(base < later_system);
  det::ordered_key later_chunk = base;
  later_chunk.chunk_index = 5;
  later_chunk.local_seq = 0;
  CHECK(base < later_chunk);
  det::ordered_key later_seq = base;
  later_seq.local_seq = 10;
  CHECK(base < later_seq);
  CHECK(base == det::ordered_key{
                    .tick = 5, .phase = 2, .system_index = 3, .chunk_index = 4, .local_seq = 9});
  CHECK(det::ordered_key_layout_version == 1);
}

TEST_CASE("RT-INV-DET-03: empty and null buffers merge to nothing; values may be moved out") {
  std::vector<det::ordered_buffer<std::vector<int>> *> none;
  int calls = 0;
  det::merge_ordered(std::span<det::ordered_buffer<std::vector<int>> *const>(none),
                     [&](const det::ordered_key &, std::vector<int> &) { ++calls; });
  CHECK(calls == 0);

  det::ordered_buffer<std::vector<int>> a;
  det::ordered_buffer<std::vector<int>> b;
  a.push(det::ordered_key{.tick = 1}, std::vector<int>{1, 1});
  b.push(det::ordered_key{.tick = 2}, std::vector<int>{2});
  a.push(det::ordered_key{.tick = 3}, std::vector<int>{3, 3, 3});
  std::array<det::ordered_buffer<std::vector<int>> *, 3> list{&b, nullptr, &a};
  std::vector<std::vector<int>> out;
  det::merge_ordered(
      std::span<det::ordered_buffer<std::vector<int>> *const>(list),
      [&](const det::ordered_key &, std::vector<int> &v) { out.push_back(std::move(v)); });
  REQUIRE(out.size() == 3);
  CHECK(out[0].size() == 2);
  CHECK(out[1].size() == 1);
  CHECK(out[2].size() == 3);
  CHECK(a.size() == 2);
  a.clear();
  CHECK(a.empty());
}

#ifndef NDEBUG
TEST_CASE("RT-INV-DET-03: a non-increasing push and a repeated key are Debug assertions") {
  int failures = 0;
  const sl::scoped_assert_handler handler([&](const sl::assert_report &) { ++failures; });
  det::ordered_buffer<int> a;
  a.push(det::ordered_key{.tick = 2}, 1);
  a.push(det::ordered_key{.tick = 2}, 2); // equal: not strictly increasing
  CHECK(failures == 1);
  det::ordered_buffer<int> x;
  det::ordered_buffer<int> y;
  x.push(det::ordered_key{.tick = 7}, 1);
  y.push(det::ordered_key{.tick = 7}, 2);
  std::array<det::ordered_buffer<int> *, 2> list{&x, &y};
  int merged = 0;
  det::merge_ordered(std::span<det::ordered_buffer<int> *const>(list),
                     [&](const det::ordered_key &, int &) { ++merged; });
  CHECK(failures >= 2);
  CHECK(merged == 2);
}
#endif
