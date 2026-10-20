// RT-INV-ALLOC-03 (SL-ARCH-003 ARCH3-R048, `limits` pool_slab_bytes and pool_min_block_bytes,
// ADR-0023): pool blocks never overlap and every freed block is on the free list exactly once over
// 1e5 seeded operations. Also: LIFO reuse, the invalid_argument and bad_alignment refusals, the
// fragmentation statistics and the Debug double-release assertion.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

// splitmix64 (no std distribution, ARCH3-R087)
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

std::uintptr_t address(const void *p) { return std::bit_cast<std::uintptr_t>(p); }

void *take(mem::resource &r, std::size_t bytes, std::size_t align = 16) {
  auto got = r.try_allocate(bytes, align, r.default_tag());
  REQUIRE(got.has_value());
  return *got;
}

} // namespace

TEST_CASE("RT-INV-ALLOC-03: pool blocks never overlap and every freed block is on the free list "
          "exactly once over 1e5 seeded operations") {
  constexpr std::uint32_t operations = 100000;
  constexpr std::size_t block = 48;
  constexpr std::size_t max_live = 3000;
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, block, 16);
  stream rng(0x5EED0840003ULL);
  std::map<std::uintptr_t, std::uintptr_t> live; // begin -> end
  std::vector<std::uintptr_t> order;
  std::uint64_t overlaps = 0;
  std::uint64_t misaligned = 0;
  std::uint64_t list_failures = 0;
  std::uint64_t count_failures = 0;
  for (std::uint32_t op = 0; op < operations; ++op) {
    const bool allocate = order.empty() || (order.size() < max_live && rng.next() % 5U < 3U);
    if (allocate) {
      const auto bytes = static_cast<std::size_t>(rng.next() % (block + 1));
      const std::size_t align = std::size_t{1} << (rng.next() % 5U);
      auto got = pool.try_allocate(bytes, align, pool.default_tag());
      REQUIRE(got.has_value());
      const std::uintptr_t begin = address(*got);
      const std::uintptr_t end = begin + pool.stride();
      misaligned += begin % mem::effective_alignment(align) != 0 ? 1U : 0U;
      const auto next = live.lower_bound(begin);
      if (next != live.end() && next->first < end) {
        ++overlaps;
      }
      if (next != live.begin() && std::prev(next)->second > begin) {
        ++overlaps;
      }
      std::memset(*got, static_cast<int>(op & 0xFFU), mem::effective_size(bytes));
      live.emplace(begin, end);
      order.push_back(begin);
    } else {
      const auto i = static_cast<std::size_t>(rng.next() % order.size());
      const std::uintptr_t begin = order[i];
      order[i] = order.back();
      order.pop_back();
      live.erase(begin);
      pool.release(std::bit_cast<void *>(begin), block, 16);
    }
    if (op % 1000U == 999U) {
      list_failures += pool.check_free_list() ? 0U : 1U;
      count_failures += pool.free_blocks() + live.size() == pool.total_blocks() ? 0U : 1U;
    }
  }
  CHECK(overlaps == 0);
  CHECK(misaligned == 0);
  CHECK(list_failures == 0);
  CHECK(count_failures == 0);
  CHECK(pool.slabs() > 1);
  for (const auto &[begin, end] : live) {
    static_cast<void>(end);
    pool.release(std::bit_cast<void *>(begin), block, 16);
  }
  CHECK(pool.free_blocks() == pool.total_blocks());
  CHECK(pool.check_free_list());
}

TEST_CASE("RT-INV-ALLOC-03: the free list is LIFO and slabs hold 64 KiB") {
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, 16, 16);
  void *a = take(pool, 16);
  void *b = take(pool, 16);
  CHECK(pool.slabs() == 1);
  CHECK(upstream.totals().live_bytes == mem::pool_slab_bytes);
  CHECK(pool.total_blocks() * pool.stride() <= mem::pool_slab_bytes);
  CHECK(pool.total_blocks() > 4000);
  pool.release(a, 16, 16);
  pool.release(b, 16, 16);
  CHECK(take(pool, 16) == b);
  CHECK(take(pool, 16) == a);
  if (mem::debug_memory && !mem::asan_enabled) {
    pool.release(a, 16, 16);
    std::byte last{};
    std::memcpy(&last, static_cast<std::byte *>(a) + 15, 1);
    CHECK(last == mem::debug_fill_freed);
    a = take(pool, 16);
  }
  pool.release(a, 16, 16);
  pool.release(b, 16, 16);
  CHECK(pool.check_free_list());
}

TEST_CASE("RT-INV-ALLOC-03: requests beyond the block are invalid_argument") {
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, 48, 16);
  CHECK(pool.block_size() == 48);
  CHECK(pool.stride() == 48);
  auto big = pool.try_allocate(49, 16, pool.default_tag());
  REQUIRE_FALSE(big.has_value());
  CHECK(big.error().code == sl::errc::invalid_argument);
  auto over_aligned = pool.try_allocate(16, 32, pool.default_tag());
  REQUIRE_FALSE(over_aligned.has_value());
  CHECK(over_aligned.error().code == sl::errc::invalid_argument);
  auto bad = pool.try_allocate(16, 3, pool.default_tag());
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::bad_alignment);
  CHECK(pool.slabs() == 0);
}

TEST_CASE("RT-INV-ALLOC-03: free_space_stats reports free bytes and the largest free run") {
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, 64, 64);
  void *first = take(pool, 64, 64);
  const std::size_t n = pool.total_blocks();
  std::vector<void *> blocks{first};
  for (std::size_t i = 1; i < n; ++i) {
    blocks.push_back(take(pool, 64, 64));
  }
  CHECK(pool.slabs() == 1);
  CHECK(pool.free_space_stats().free_bytes == 0);
  CHECK(pool.free_space_stats().largest_free_run == 0);
  for (const std::size_t i : {std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{10}}) {
    pool.release(blocks[i], 64, 64);
  }
  const mem::free_space f = pool.free_space_stats();
  CHECK(f.free_bytes == 4 * pool.stride());
  CHECK(f.largest_free_run == 3 * pool.stride());
  CHECK(pool.check_free_list());
  for (std::size_t i = 0; i < n; ++i) {
    if (i != 2 && i != 3 && i != 4 && i != 10) {
      pool.release(blocks[i], 64, 64);
    }
  }
  CHECK(pool.free_space_stats().largest_free_run == n * pool.stride());
}

TEST_CASE("RT-INV-ALLOC-03: a double release trips the Debug assertion and changes nothing") {
#ifdef NDEBUG
  MESSAGE("skipped: the double-release check is a Debug assertion");
#else
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, 32, 16);
  void *a = take(pool, 32);
  pool.release(a, 32, 16);
  const std::size_t free_before = pool.free_blocks();
  int fired = 0;
  {
    const sl::scoped_assert_handler h([&fired](const sl::assert_report &) { ++fired; });
    pool.release(a, 32, 16);
    int local = 0;
    pool.release(&local, 32, 16);
  }
  CHECK(fired == 2);
  CHECK(pool.free_blocks() == free_before);
  CHECK(pool.check_free_list());
#endif
}
