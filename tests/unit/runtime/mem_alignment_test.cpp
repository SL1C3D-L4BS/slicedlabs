// RT-INV-MEM-01 (SL-ARCH-003 ARCH3-R043, ADR-0023): every resource honours each power-of-two
// alignment up to 4096, refuses others with bad_alignment and serves a zero-byte request. The
// resources of this volume so far are tracking_resource (over new_delete, over a monotonic buffer
// and over another tracking_resource) and testing::failing_resource; each also passes the
// reusable allocator contract suite (testing/allocator_contract.hpp).

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/allocator_contract.hpp"
#include "../../../engine/runtime/testing/failing_resource.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

std::string joined(const std::vector<std::string> &messages) {
  std::string out;
  for (const std::string &m : messages) {
    out += m;
    out += '\n';
  }
  return out;
}

bool aligned_to(const void *p, std::size_t align) {
  return std::bit_cast<std::uintptr_t>(p) % align == 0;
}

void check_alignments(mem::resource &r) {
  constexpr std::array<std::size_t, 4> sizes = {0, 1, 33, 4096};
  for (std::size_t align = 1; align <= mem::max_alignment; align <<= 1U) {
    for (const std::size_t bytes : sizes) {
      auto got = r.try_allocate(bytes, align, r.default_tag());
      REQUIRE(got.has_value());
      CHECK(*got != nullptr);
      CHECK(aligned_to(*got, mem::effective_alignment(align)));
      CHECK(aligned_to(*got, alignof(std::max_align_t)));
      r.release(*got, bytes, align);
    }
  }
  constexpr std::array<std::size_t, 6> bad = {0, 3, 12, 96, 8192, 65536};
  for (const std::size_t align : bad) {
    auto got = r.try_allocate(16, align, r.default_tag());
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::bad_alignment);
  }
}

} // namespace

TEST_CASE("RT-INV-MEM-01: is_valid_alignment accepts the powers of two up to 4096 only") {
  static_assert(mem::is_valid_alignment(1) && mem::is_valid_alignment(16) &&
                mem::is_valid_alignment(4096));
  static_assert(!mem::is_valid_alignment(0) && !mem::is_valid_alignment(3) &&
                !mem::is_valid_alignment(8192));
  static_assert(mem::effective_alignment(1) == alignof(std::max_align_t));
  static_assert(mem::effective_alignment(4096) == 4096);
  static_assert(mem::effective_size(0) == 1 && mem::effective_size(5) == 5);
  for (std::size_t a = 0; a <= 9000; ++a) {
    CHECK(mem::is_valid_alignment(a) == (std::has_single_bit(a) && a <= 4096));
  }
}

TEST_CASE("RT-INV-MEM-01: tracking_resource honours every power-of-two alignment and refuses "
          "others with bad_alignment") {
  mem::tag_registry tags;
  const auto root = tags.add("runtime.root");
  REQUIRE(root.has_value());
  mem::tracking_resource r("root", *root, &tags);
  check_alignments(r);
  CHECK(r.counters(*root).live_bytes == 0);
  CHECK(r.counters(*root).failures == 6);
}

TEST_CASE("RT-INV-MEM-01: a zero-byte request is served as one distinct byte") {
  mem::tracking_resource r("zero");
  auto a = r.try_allocate(0, 1, r.default_tag());
  auto b = r.try_allocate(0, 1, r.default_tag());
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(*a != *b);
  CHECK(r.totals().live_bytes == 2);
  CHECK(r.totals().live_blocks == 2);
  r.release(*a, 0, 1);
  r.release(*b, 0, 1);
  CHECK(r.totals().live_bytes == 0);
}

TEST_CASE("RT-INV-MEM-01: failing_resource that never fails honours the alignments") {
  rtt::failing_resource f(*std::pmr::new_delete_resource(), 0);
  check_alignments(f);
}

TEST_CASE("RT-INV-MEM-01: every resource passes the allocator contract suite") {
  const rtt::contract_options options{.seed = 83, .operations = 2000, .owner_thread_only = true};
  {
    mem::tracking_resource r("contract.new_delete");
    const auto failures = rtt::run_allocator_contract(r, options);
    CHECK_MESSAGE(failures.empty(), joined(failures));
  }
  {
    std::pmr::monotonic_buffer_resource mono;
    mem::tracking_resource r("contract.monotonic", {}, nullptr, &mono);
    const auto failures = rtt::run_allocator_contract(r, options);
    CHECK_MESSAGE(failures.empty(), joined(failures));
  }
  {
    mem::tracking_resource outer("contract.outer");
    mem::tracking_resource inner("contract.inner", {}, nullptr, &outer);
    const auto failures = rtt::run_allocator_contract(inner, options);
    CHECK_MESSAGE(failures.empty(), joined(failures));
    CHECK(outer.totals().live_bytes == 0);
    CHECK(outer.totals().allocs == inner.totals().allocs);
  }
  {
    mem::tracking_resource below("contract.below");
    rtt::failing_resource f(below, 0);
    const auto failures = rtt::run_allocator_contract(f, options);
    CHECK_MESSAGE(failures.empty(), joined(failures));
    CHECK(below.totals().live_bytes == 0);
    CHECK(below.totals().allocs == below.totals().frees);
  }
}
