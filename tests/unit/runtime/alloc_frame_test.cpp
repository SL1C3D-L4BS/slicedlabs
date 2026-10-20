// RT-INV-ALLOC-04 (SL-ARCH-003 ARCH3-R050, R051, `thread_safety` frame_arena rows, ADR-0023):
// frame-N memory survives the end_frame of N and is reset, filled 0xDD (Debug) and poisoned (ASan)
// by the flip that ends N+1. Workers allocate from their own arenas on their own threads (joined
// before each flip, as the job system drains before end_frame); after warm-up a steady frame takes
// no upstream block.

#include "sl/core/assert.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

bool poisoned(const void *p) {
#if SL_RUNTIME_ASAN
  return __asan_address_is_poisoned(p) != 0;
#else
  static_cast<void>(p);
  return false;
#endif
}

std::vector<std::byte> peek(const void *p, std::size_t n) {
  const bool was = poisoned(p);
  if (was) {
    mem::unpoison(p, n);
  }
  std::vector<std::byte> out(n);
  std::memcpy(out.data(), p, n);
  if (was) {
    mem::poison(p, n);
  }
  return out;
}

bool all_equal(const std::vector<std::byte> &bytes, std::byte v) {
  return std::ranges::all_of(bytes, [v](std::byte b) { return b == v; });
}

void *take(mem::resource &r, std::size_t bytes, int pattern) {
  auto got = r.try_allocate(bytes, 16, r.default_tag());
  REQUIRE(got.has_value());
  std::memset(*got, pattern, bytes);
  return *got;
}

// Allocates `bytes` from worker k's arena on a thread of its own and joins it.
void *take_on_worker(mem::frame_arena &frames, std::uint32_t k, std::size_t bytes, int pattern) {
  void *p = nullptr;
  std::thread worker([&frames, k, bytes, pattern, &p] {
    auto got = frames.for_worker(k).try_allocate(bytes, 16, frames.default_tag());
    if (got.has_value()) {
      std::memset(*got, pattern, bytes);
      p = *got;
    }
  });
  worker.join();
  REQUIRE(p != nullptr);
  return p;
}

void require_reset(const void *p, std::size_t n) {
  if (mem::asan_enabled) {
    CHECK(poisoned(p));
  }
  if (mem::debug_memory) {
    CHECK(all_equal(peek(p, n), mem::debug_fill_freed));
  }
}

} // namespace

TEST_CASE("RT-INV-ALLOC-04: frame-N memory survives the end_frame of N and is reset by the flip "
          "that ends N+1") {
  mem::tracking_resource upstream("upstream");
  mem::frame_arena frames(upstream, 2, 4096);
  CHECK(frames.workers() == 2);
  // frame 0
  const std::uint32_t set0 = frames.current_set();
  void *owner0 = take(frames, 256, 0x11);
  void *worker0 = take_on_worker(frames, 0, 256, 0x22);
  void *worker1 = take_on_worker(frames, 1, 512, 0x23);
  void *owner_arena0 = take(frames.for_worker(2), 128, 0x24);
  frames.flip(); // end_frame of 0
  CHECK(frames.current_set() != set0);
  CHECK(all_equal(peek(owner0, 256), std::byte{0x11}));
  CHECK(all_equal(peek(worker0, 256), std::byte{0x22}));
  CHECK(all_equal(peek(worker1, 512), std::byte{0x23}));
  CHECK(all_equal(peek(owner_arena0, 128), std::byte{0x24}));
  CHECK_FALSE(poisoned(owner0));
  CHECK_FALSE(poisoned(worker0));
  // frame 1
  void *owner1 = take(frames, 256, 0x33);
  void *worker1_0 = take_on_worker(frames, 0, 256, 0x34);
  CHECK(all_equal(peek(owner0, 256), std::byte{0x11})); // still valid during frame 1
  frames.flip();                                        // end_frame of 1: frame 0 is reset
  CHECK(frames.current_set() == set0);
  require_reset(owner0, 256);
  require_reset(worker0, 256);
  require_reset(worker1, 512);
  require_reset(owner_arena0, 128);
  CHECK(all_equal(peek(owner1, 256), std::byte{0x33}));
  CHECK(all_equal(peek(worker1_0, 256), std::byte{0x34}));
  CHECK_FALSE(poisoned(owner1));
  // frame 2 reuses frame 0's memory
  CHECK(take(frames, 256, 0x44) == owner0);
  frames.flip(); // end_frame of 2: frame 1 is reset
  require_reset(owner1, 256);
  require_reset(worker1_0, 256);
  CHECK(frames.flips() == 3);
}

TEST_CASE("RT-INV-ALLOC-04: after warm-up a steady frame takes no upstream block") {
  mem::tracking_resource upstream("upstream");
  mem::frame_arena frames(upstream, 2, 4096);
  std::uint64_t after_warmup = 0;
  for (int frame = 0; frame < 30; ++frame) {
    for (int i = 0; i < 40; ++i) {
      static_cast<void>(take(frames, 200, frame)); // grows past one 4096-byte block
    }
    static_cast<void>(take_on_worker(frames, 0, 3000, frame));
    static_cast<void>(take_on_worker(frames, 1, 6000, frame)); // larger than block_bytes
    frames.flip();
    if (frame == 9) {
      after_warmup = frames.upstream_allocations();
    }
  }
  CHECK(after_warmup > 0);
  CHECK(frames.upstream_allocations() == after_warmup);
  CHECK(frames.flips() == 30);
}

TEST_CASE("RT-INV-ALLOC-04: a worker arena binds its owner at the first allocation after a flip") {
#ifdef NDEBUG
  MESSAGE("skipped: the owner check is a Debug assertion");
#else
  mem::tracking_resource upstream("upstream");
  mem::frame_arena frames(upstream, 1, 4096);
  int fired = 0;
  const sl::scoped_assert_handler h([&fired](const sl::assert_report &) { ++fired; });
  static_cast<void>(take_on_worker(frames, 0, 64, 1)); // thread A binds worker 0 of set A
  frames.flip();
  static_cast<void>(take_on_worker(frames, 0, 64, 2)); // thread B binds worker 0 of set B
  frames.flip();
  static_cast<void>(take_on_worker(frames, 0, 64, 3)); // set A was reset: thread C binds it
  CHECK(fired == 0);
#endif
}
