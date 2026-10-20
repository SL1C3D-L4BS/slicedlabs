// RT-INV-ALLOC-01 (SL-ARCH-003 ARCH3-R048, ADR-0023): the allocator contract suite passes on all
// six resources. Each resource sits on a tracking_resource, runs the contract three times with
// different seeds and, once destroyed, has returned every upstream byte.

#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/allocator_contract.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::uint64_t base_seed = 0x5EED0840001ULL;

void require_contract(mem::resource &r) {
  for (std::uint64_t s = 0; s < 3; ++s) {
    const std::vector<std::string> failures = rtt::run_allocator_contract(
        r, rtt::contract_options{.seed = base_seed + s, .operations = 1000});
    for (const std::string &f : failures) {
      MESSAGE(f);
    }
    CHECK(failures.empty());
  }
}

void require_balanced(const mem::tracking_resource &upstream) {
  const mem::tag_counters t = upstream.totals();
  CHECK(t.live_bytes == 0);
  CHECK(t.live_blocks == 0);
  CHECK(t.allocs == t.frees);
}

} // namespace

TEST_CASE("RT-INV-ALLOC-01: arena_resource passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::arena_resource arena(upstream);
    require_contract(arena);
    CHECK(arena.upstream_allocations() > 0);
    // small blocks: every large request takes an oversized block of its own
    mem::arena_resource small(upstream, 256);
    require_contract(small);
  }
  require_balanced(upstream);
}

TEST_CASE("RT-INV-ALLOC-01: stack_resource passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::stack_resource stack(upstream, std::size_t{4} << 20U);
    require_contract(stack);
    // every block was released, out of order: the deferred releases returned all the space
    CHECK(stack.used() == 0);
    CHECK(stack.depth() == 0);
  }
  require_balanced(upstream);
}

TEST_CASE("RT-INV-ALLOC-01: pool_resource passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::pool_resource pool(upstream, 4104, 4096);
    require_contract(pool);
    CHECK(pool.free_blocks() == pool.total_blocks());
    CHECK(pool.check_free_list());
  }
  require_balanced(upstream);
}

TEST_CASE("RT-INV-ALLOC-01: frame_arena passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::frame_arena frames(upstream, 2, 65536);
    require_contract(frames);
    frames.flip();
    require_contract(frames);
    frames.flip();
    frames.flip();
    CHECK(frames.used() == 0);
    CHECK(frames.flips() == 3);
  }
  require_balanced(upstream);
}

TEST_CASE("RT-INV-ALLOC-01: synchronized_resource passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::pool_resource pool(upstream, 4104, 4096);
    mem::synchronized_resource over_pool(pool);
    require_contract(over_pool);
    CHECK(pool.shared());
    mem::arena_resource arena(upstream);
    mem::synchronized_resource over_arena(arena);
    require_contract(over_arena);
  }
  require_balanced(upstream);
}

TEST_CASE("RT-INV-ALLOC-01: general_resource passes the allocator contract") {
  mem::tracking_resource upstream("upstream");
  {
    mem::general_resource general(&upstream);
    require_contract(general);
    CHECK(general.upstream_resource() == &upstream);
  }
  require_balanced(upstream);
  mem::general_resource heap;
  CHECK(heap.upstream_resource() == std::pmr::new_delete_resource());
  require_contract(heap);
}
