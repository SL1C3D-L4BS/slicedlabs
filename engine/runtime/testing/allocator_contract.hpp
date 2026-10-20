#pragma once

// The reusable allocator contract suite (WO-079, SL-ARCH-003 ARCH3-R041 to R045, ADR-0023): every
// mem::resource must pass it (WO-081 runs it against all six allocators). It needs no doctest: it
// returns one message per failed check, empty when the resource passes. Steps: an alignment sweep
// over every power of two up to 4096 with the blocks held together (aligned, writable, disjoint),
// the refusal of other alignments with bad_alignment, zero-byte requests, the pmr path and
// identity, `operations` seeded allocate/verify/release operations, release of every block and,
// when the resource is a tracking_resource used by this suite alone, exact tag accounting. It
// creates no thread (ARCH3-R035); a test runs it from several threads with owner_thread_only
// false. Include it by path: "../../../engine/runtime/testing/allocator_contract.hpp".

#include "sl/runtime/memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sl::runtime::testing {

struct contract_options {
  /// The seed of the operation sequence (a splitmix64 stream).
  std::uint64_t seed = 1;
  /// The number of seeded operations.
  std::uint32_t operations = 1000;
  /// True: this suite is the resource's only user, so the counters of a tracking_resource must
  /// move exactly by what the suite does. False: other threads may use the resource at the same
  /// time; the counter checks are skipped and the steps that need exclusivity still pass.
  bool owner_thread_only = true;
};

/// Runs the contract against `r` (allocations carry r.default_tag()); one message per failure.
[[nodiscard]] std::vector<std::string> run_allocator_contract(mem::resource &r,
                                                              const contract_options &options);

} // namespace sl::runtime::testing
