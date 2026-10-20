// RT-INV-ALLOC-02 (SL-ARCH-003 ARCH3-R048, assertions list, ADR-0023): arena rewind to a marker
// frees exactly the later allocations, reset returns usage to 0, and an out-of-order stack free
// trips the captured assertion. Freed ranges read 0xDD (Debug) and are poisoned (ASan).

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
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

// The bytes of [p, p + n), read through a temporary unpoison when the range is poisoned.
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

void *take(mem::resource &r, std::size_t bytes, std::size_t align = 16) {
  auto got = r.try_allocate(bytes, align, r.default_tag());
  REQUIRE(got.has_value());
  return *got;
}

struct capture {
  int fired = 0;
  std::string message;
};

} // namespace

TEST_CASE("RT-INV-ALLOC-02: arena rewind to a marker frees exactly the later allocations") {
  mem::tracking_resource upstream("upstream");
  mem::arena_resource arena(upstream, 1024);
  std::vector<void *> before;
  for (int i = 0; i < 5; ++i) {
    before.push_back(take(arena, 96));
    std::memset(before.back(), 0x40 + i, 96);
  }
  const mem::arena_resource::marker m = arena.mark();
  const std::size_t used_at_mark = arena.used();
  std::vector<void *> after;
  for (int i = 0; i < 20; ++i) {
    after.push_back(take(arena, 96));
    std::memset(after.back(), 0x60, 96);
  }
  CHECK(arena.used() > used_at_mark);
  const std::uint64_t blocks = arena.upstream_allocations();
  CHECK(blocks > 1); // the later allocations crossed into new blocks

  arena.rewind(m);
  CHECK(arena.used() == used_at_mark);
  for (int i = 0; i < 5; ++i) {
    CHECK(all_equal(peek(before[static_cast<std::size_t>(i)], 96), std::byte(0x40 + i)));
    CHECK_FALSE(poisoned(before[static_cast<std::size_t>(i)]));
  }
  for (void *p : after) {
    if (mem::asan_enabled) {
      CHECK(poisoned(p));
    }
    if (mem::debug_memory) {
      CHECK(all_equal(peek(p, 96), mem::debug_fill_freed));
    }
  }
  // exactly the later allocations are free: the same requests get the same addresses back
  for (void *p : after) {
    CHECK(take(arena, 96) == p);
  }
  CHECK(arena.upstream_allocations() == blocks);
}

TEST_CASE("RT-INV-ALLOC-02: arena reset returns usage to 0 and keeps the blocks") {
  mem::tracking_resource upstream("upstream");
  mem::arena_resource arena(upstream, 4096);
  void *first = take(arena, 200);
  for (int i = 0; i < 100; ++i) {
    static_cast<void>(take(arena, 128, 64));
  }
  const std::uint64_t blocks = arena.upstream_allocations();
  const std::size_t reserved = arena.reserved();
  CHECK(arena.used() > 0);
  arena.reset();
  CHECK(arena.used() == 0);
  if (mem::asan_enabled) {
    CHECK(poisoned(first));
  }
  CHECK(take(arena, 200) == first);
  CHECK(arena.upstream_allocations() == blocks);
  CHECK(arena.reserved() == reserved);
  arena.reset();
  CHECK(arena.used() == 0);
}

TEST_CASE("RT-INV-ALLOC-02: arena release frees only the LIFO tail") {
  mem::tracking_resource upstream("upstream");
  mem::arena_resource arena(upstream);
  void *a = take(arena, 64);
  void *b = take(arena, 64);
  const std::size_t used_two = arena.used();
  arena.release(b, 64, 16);
  CHECK(arena.used() < used_two);
  CHECK(take(arena, 64) == b);
  arena.release(a, 64, 16); // not the tail: no space comes back
  CHECK(arena.used() == used_two);
  if (mem::asan_enabled) {
    CHECK(poisoned(a));
  }
}

TEST_CASE("RT-INV-ALLOC-02: a fixed-capacity arena reports arena_exhausted") {
  mem::tracking_resource upstream("upstream");
  mem::arena_resource arena(upstream, 4096, 8192);
  int served = 0;
  sl::errc last = sl::errc::ok;
  for (int i = 0; i < 100; ++i) {
    auto got = arena.try_allocate(1000, 16, arena.default_tag());
    if (!got.has_value()) {
      last = got.error().code;
      break;
    }
    ++served;
  }
  CHECK(served >= 6);
  CHECK(last == sl::errc::arena_exhausted);
  CHECK(arena.reserved() <= 8192);
  auto bad = arena.try_allocate(8, 3, arena.default_tag());
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::bad_alignment);
}

TEST_CASE("RT-INV-ALLOC-02: an out-of-order stack free trips the captured assertion") {
  mem::tracking_resource upstream("upstream");
  mem::stack_resource stack(upstream, 65536);
  void *below = take(stack, 32);
  REQUIRE(stack.push_marker().has_value());
  CHECK(stack.depth() == 1);
  const std::size_t at_marker = stack.used();
  void *a = take(stack, 64);
  void *b = take(stack, 64);
  std::memset(a, 0x11, 64);
  capture c;
  {
    const sl::scoped_assert_handler h([&c](const sl::assert_report &r) {
      ++c.fired;
      c.message = std::string(r.message);
    });
    stack.release(a, 64, 16);     // b is the top
    stack.release(below, 32, 16); // below the marker
  }
  CHECK(c.fired == 2);
  CHECK(c.message.find("out-of-order") != std::string::npos);
  // the refused releases changed nothing
  CHECK(all_equal(peek(a, 64), std::byte{0x11}));
  stack.release(b, 64, 16);
  stack.release(a, 64, 16);
  CHECK(stack.used() == at_marker);
  stack.pop_marker();
  CHECK(stack.depth() == 0);
  stack.release(below, 32, 16);
  CHECK(stack.used() == 0);
}

TEST_CASE("RT-INV-ALLOC-02: stack markers free their scope; releases outside a scope defer") {
  mem::tracking_resource upstream("upstream");
  mem::stack_resource stack(upstream, 65536);
  REQUIRE(stack.push_marker().has_value());
  static_cast<void>(take(stack, 100));
  const std::size_t at_inner = stack.used();
  REQUIRE(stack.push_marker().has_value());
  void *inner = take(stack, 100);
  static_cast<void>(take(stack, 300, 256));
  CHECK(stack.depth() == 2);
  stack.pop_marker(); // frees inner and the 300 bytes without a release
  CHECK(stack.depth() == 1);
  CHECK(stack.used() == at_inner);
  if (mem::asan_enabled) {
    CHECK(poisoned(inner));
  }
  if (mem::debug_memory) {
    CHECK(all_equal(peek(inner, 100), mem::debug_fill_freed));
  }
  void *again = take(stack, 100); // the space came back
  CHECK(std::bit_cast<std::uintptr_t>(again) < std::bit_cast<std::uintptr_t>(inner));
  stack.release(again, 100, 16);
  CHECK(stack.used() == at_inner);
  stack.pop_marker();
  CHECK(stack.depth() == 0);
  CHECK(stack.used() == 0);

  // depth 0: a release that is not the top waits for the blocks above it
  void *x = take(stack, 48);
  void *y = take(stack, 48);
  void *z = take(stack, 48);
  stack.release(x, 48, 16);
  stack.release(y, 48, 16);
  CHECK(stack.used() > 0);
  if (mem::debug_memory) {
    CHECK(all_equal(peek(x, 48), mem::debug_fill_freed));
  }
  stack.release(z, 48, 16);
  CHECK(stack.used() == 0);

  capture c;
  {
    const sl::scoped_assert_handler h([&c](const sl::assert_report &r) {
      ++c.fired;
      c.message = std::string(r.message);
    });
    stack.pop_marker();
  }
  CHECK(c.fired == 1);
  CHECK(c.message.find("pop_marker") != std::string::npos);
}

TEST_CASE("RT-INV-ALLOC-02: a full stack reports arena_exhausted") {
  mem::tracking_resource upstream("upstream");
  mem::stack_resource stack(upstream, 4096);
  int served = 0;
  sl::errc last = sl::errc::ok;
  for (int i = 0; i < 100; ++i) {
    auto got = stack.try_allocate(500, 16, stack.default_tag());
    if (!got.has_value()) {
      last = got.error().code;
      break;
    }
    ++served;
  }
  CHECK(served == 7);
  CHECK(last == sl::errc::arena_exhausted);
  CHECK(stack.free_space_stats().free_bytes == stack.capacity() - stack.used());
}
