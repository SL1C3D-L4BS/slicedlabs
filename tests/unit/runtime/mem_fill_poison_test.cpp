// RT-INV-MEM-06 (SL-ARCH-003 ARCH3-R046, `limits` debug_fill_fresh/debug_fill_freed, ADR-0023):
// Debug fill patterns 0xCD and 0xDD are present on fresh and freed blocks, and under ASan freed
// and reset ranges are poisoned (skips with a message elsewhere). The tracking_resource sits on a
// monotonic buffer, so a freed block stays owned by the test and can be read after release.

#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

bool all_bytes(const void *p, std::size_t n, std::byte value) {
  const std::span<const std::byte> bytes(static_cast<const std::byte *>(p), n);
  return std::ranges::all_of(bytes, [value](std::byte b) { return b == value; });
}

bool poisoned(const std::byte *p) {
#if SL_RUNTIME_ASAN
  return __asan_address_is_poisoned(p) != 0;
#else
  static_cast<void>(p);
  return false;
#endif
}

} // namespace

TEST_CASE("RT-INV-MEM-06: fresh blocks hold 0xCD and freed blocks 0xDD") {
  static_assert(mem::debug_fill_fresh == std::byte{0xCD});
  static_assert(mem::debug_fill_freed == std::byte{0xDD});
  if (!mem::debug_memory) {
    MESSAGE("skipped: the Debug fills are off in Release");
    return;
  }
  std::vector<std::byte> buffer(std::size_t{1} << 15U);
  std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size(),
                                            std::pmr::null_memory_resource());
  mem::tracking_resource r("fill", {}, nullptr, &arena);
  auto got = r.try_allocate(256, 64, r.default_tag());
  REQUIRE(got.has_value());
  CHECK(all_bytes(*got, 256, mem::debug_fill_fresh));
  std::ranges::fill(std::span<std::byte>(static_cast<std::byte *>(*got), 256), std::byte{0x11});
  r.release(*got, 256, 64);
  CHECK(all_bytes(*got, 256, mem::debug_fill_freed));

  auto zero = r.try_allocate(0, 1, r.default_tag());
  REQUIRE(zero.has_value());
  CHECK(all_bytes(*zero, 1, mem::debug_fill_fresh));
  r.release(*zero, 0, 1);
  CHECK(all_bytes(*zero, 1, mem::debug_fill_freed));

  std::vector<std::byte> raw(64, std::byte{0});
  mem::fill_fresh(raw.data(), raw.size());
  CHECK(all_bytes(raw.data(), raw.size(), mem::debug_fill_fresh));
  mem::fill_freed(raw.data(), raw.size());
  CHECK(all_bytes(raw.data(), raw.size(), mem::debug_fill_freed));
}

TEST_CASE("RT-INV-MEM-06: under ASan freed and reset ranges are poisoned") {
  if (!mem::asan_enabled) {
    MESSAGE("skipped: poisoning is checked in the ASan build only");
    return;
  }
  // a recycling resource's buffer: [0, 64) live, [64, 128) freed, then the whole range reset
  std::vector<std::byte> buffer(256);
  std::byte *base = buffer.data();
  mem::poison(base + 64, 64);
  for (std::size_t i = 0; i < 256; ++i) {
    CHECK(poisoned(base + i) == (i >= 64 && i < 128));
  }
  mem::unpoison(base + 64, 64);
  CHECK_FALSE(poisoned(base + 64));
  mem::poison(base, 256);
  for (std::size_t i = 0; i < 256; ++i) {
    CHECK(poisoned(base + i));
  }
  mem::unpoison(base, 256);
  for (std::size_t i = 0; i < 256; ++i) {
    CHECK_FALSE(poisoned(base + i));
  }
}
