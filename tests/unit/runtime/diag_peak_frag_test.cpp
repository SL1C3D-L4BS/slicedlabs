// RT-INV-DIAG-01 (SL-ARCH-003 ARCH3-R052, R056, ADR-0023): the per-tag peak of a memory report
// equals the oracle maximum of live bytes over a seeded alloc/free sequence, fragmentation
// per-mille equals 1000 - 1000 x largest free run / free bytes on planted layouts (a resource with
// set free space, and a real pool with every other block free), and bytes reported through
// report_external never enter the tracked live bytes.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <source_location>
#include <span>
#include <string>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

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

// A resource whose free space is set by the test (the planted layout).
class planted_resource final : public mem::resource {
public:
  planted_resource() : mem::resource("planted") {}
  [[nodiscard]] sl::result<void *> try_allocate(std::size_t /*bytes*/, std::size_t /*align*/,
                                                mem::memory_tag /*tag*/,
                                                std::source_location /*site*/) noexcept override {
    return sl::make_error(sl::errc::out_of_memory, "planted: never allocates");
  }
  void release(void * /*p*/, std::size_t /*bytes*/, std::size_t /*align*/) noexcept override {}
  [[nodiscard]] mem::free_space free_space_stats() const noexcept override { return layout; }

  mem::free_space layout;
};

const mem::tag_stats *find_tag(const mem::memory_report &r, const std::string &name) {
  const auto it = std::ranges::find(r.tags, name, &mem::tag_stats::tag);
  return it == r.tags.end() ? nullptr : &*it;
}

std::uint32_t oracle_permille(std::uint64_t free_bytes, std::uint64_t run) {
  return free_bytes == 0 ? 0U : static_cast<std::uint32_t>(1000U - ((1000U * run) / free_bytes));
}

struct oracle_tag {
  std::uint64_t live = 0;
  std::uint64_t blocks = 0;
  std::uint64_t peak = 0;
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
};

struct block {
  void *p = nullptr;
  std::size_t size = 0;
  std::size_t tag_index = 0;
};

} // namespace

TEST_CASE("RT-INV-DIAG-01: peak bytes equal the oracle maximum of live bytes") {
  mem::tag_registry registry;
  const auto chunks = registry.add("ecs.chunks");
  const auto events = registry.add("events");
  REQUIRE(chunks.has_value());
  REQUIRE(events.has_value());
  REQUIRE(registry.freeze().has_value());
  const std::array<mem::memory_tag, 2> tags{*chunks, *events};
  const std::array<std::string, 2> names{"ecs.chunks", "events"};

  mem::tracking_resource tracked("tracked", *chunks, &registry);
  mem::diagnostics diag(registry, 1);
  diag.add(tracked);

  stream rng(0xD1A6001ULL);
  std::array<oracle_tag, 2> oracle{};
  std::vector<block> live;
  for (int step = 0; step < 3000; ++step) {
    const bool grow = live.empty() || rng.next() % 100U < 55U;
    if (grow) {
      const std::size_t t = rng.next() % 2U;
      const std::size_t size = 1 + (rng.next() % 4096U);
      auto p = tracked.try_allocate(size, 16, std::span(tags)[t]);
      REQUIRE(p.has_value());
      live.push_back(block{.p = *p, .size = size, .tag_index = t});
      oracle_tag &o = std::span(oracle)[t];
      o.live += size;
      o.blocks += 1;
      o.allocs += 1;
      o.peak = std::max(o.peak, o.live);
    } else {
      const std::size_t i = rng.next() % live.size();
      const block b = live[i];
      live[i] = live.back();
      live.pop_back();
      tracked.release(b.p, b.size, 16);
      oracle_tag &o = std::span(oracle)[b.tag_index];
      o.live -= b.size;
      o.blocks -= 1;
      o.frees += 1;
    }
    if (step % 250 == 0 || step == 2999) {
      const mem::memory_report report = diag.make_report(static_cast<std::uint64_t>(step));
      REQUIRE(report.tags.size() == 2);
      for (std::size_t t = 0; t < 2; ++t) {
        const mem::tag_stats *s = find_tag(report, std::span(names)[t]);
        REQUIRE(s != nullptr);
        const oracle_tag &o = std::span(oracle)[t];
        CHECK(s->live_bytes == o.live);
        CHECK(s->live_blocks == o.blocks);
        CHECK(s->peak_bytes == o.peak);
        CHECK(s->allocs == o.allocs);
        CHECK(s->frees == o.frees);
        CHECK(s->failures == 0);
      }
    }
  }
  for (const block &b : live) {
    tracked.release(b.p, b.size, 16);
  }
  const mem::memory_report done = diag.make_report(3000);
  for (const mem::tag_stats &s : done.tags) {
    CHECK(s.live_bytes == 0);
    CHECK(s.allocs == s.frees);
  }
  // a failed request counts under its tag
  auto bad = tracked.try_allocate(8, 3, *events);
  CHECK_FALSE(bad.has_value());
  const mem::memory_report failed = diag.make_report(3001);
  const mem::tag_stats *ev = find_tag(failed, "events");
  REQUIRE(ev != nullptr);
  CHECK(ev->failures == 1);
}

TEST_CASE("RT-INV-DIAG-01: fragmentation per-mille equals the oracle on planted layouts") {
  mem::tag_registry registry;
  REQUIRE(registry.freeze().has_value());
  planted_resource planted;
  mem::diagnostics diag(registry, 1);
  diag.add("planted", planted);

  // {free bytes, largest free run}
  const std::array<std::array<std::uint64_t, 2>, 9> layouts{{{0, 0},
                                                             {4096, 4096},
                                                             {4096, 2048},
                                                             {3, 1},
                                                             {1000, 1},
                                                             {2015232, 1966080},
                                                             {65536, 16},
                                                             {7, 6},
                                                             {1ULL << 40U, 1ULL << 20U}}};
  std::uint64_t frame = 1;
  for (const auto &[free_bytes, run] : layouts) {
    planted.layout = mem::free_space{.free_bytes = free_bytes, .largest_free_run = run};
    diag.on_frame_end(frame);
    const mem::memory_report report = diag.make_report(frame);
    ++frame;
    REQUIRE(report.allocators.size() == 1);
    const mem::allocator_stats &a = report.allocators.front();
    CHECK(a.name == "planted");
    CHECK(a.free_bytes == free_bytes);
    CHECK(a.largest_free_run == run);
    CHECK(a.fragmentation_permille == oracle_permille(free_bytes, run));
    CHECK(a.fragmentation_permille == mem::fragmentation_permille(free_bytes, run));
  }
  CHECK(mem::fragmentation_permille(2015232, 1966080) == 25); // the schema example
  CHECK(mem::fragmentation_permille(0, 5) == 0);
  CHECK(mem::fragmentation_permille(10, 20) == 0); // a run above the free bytes counts as all
  CHECK(mem::fragmentation_permille(~0ULL, 1ULL << 62U) == 750);
}

TEST_CASE("RT-INV-DIAG-01: a pool with every other block free reports the oracle fragmentation") {
  mem::tag_registry registry;
  const auto chunks = registry.add("ecs.chunks");
  REQUIRE(chunks.has_value());
  REQUIRE(registry.freeze().has_value());
  mem::tracking_resource upstream("upstream", *chunks, &registry);
  mem::pool_resource pool(upstream, 64, 16, *chunks);

  std::vector<void *> blocks;
  auto first = pool.try_allocate(64, 16, *chunks);
  REQUIRE(first.has_value());
  blocks.push_back(*first);
  const std::size_t n = pool.total_blocks();
  REQUIRE(n >= 4);
  while (blocks.size() < n) {
    auto p = pool.try_allocate(64, 16, *chunks);
    REQUIRE(p.has_value());
    blocks.push_back(*p);
  }
  REQUIRE(pool.slabs() == 1);
  REQUIRE(pool.free_blocks() == 0);
  std::ranges::sort(blocks, std::less<>{},
                    [](void *p) { return std::bit_cast<std::uintptr_t>(p); });
  std::size_t freed = 0;
  for (std::size_t i = 0; i < blocks.size(); i += 2) {
    pool.release(blocks[i], 64, 16);
    ++freed;
  }

  mem::diagnostics diag(registry, 1);
  diag.add("ecs.chunk_pool", pool); // sampled now, on the pool's owner thread
  const mem::memory_report report = diag.make_report(1);
  REQUIRE(report.allocators.size() == 1);
  const mem::allocator_stats &a = report.allocators.front();
  const std::uint64_t free_bytes = freed * pool.stride();
  CHECK(a.free_bytes == free_bytes);
  CHECK(a.largest_free_run == pool.stride());
  CHECK(a.fragmentation_permille == oracle_permille(free_bytes, pool.stride()));
  CHECK(a.fragmentation_permille > 900);

  for (std::size_t i = 1; i < blocks.size(); i += 2) {
    pool.release(blocks[i], 64, 16);
  }
  diag.on_frame_end(2);
  const mem::allocator_stats all_free = diag.make_report(2).allocators.front();
  CHECK(all_free.free_bytes == n * pool.stride());
  CHECK(all_free.largest_free_run == n * pool.stride());
  CHECK(all_free.fragmentation_permille == 0);
}

TEST_CASE("RT-INV-DIAG-01: external tags never enter tracked live bytes") {
  mem::tag_registry registry;
  const auto events = registry.add("events");
  const auto projection = registry.reserve_external("fabric.projection");
  const auto textures = registry.reserve_external("gpu.textures");
  REQUIRE(events.has_value());
  REQUIRE(projection.has_value());
  REQUIRE(textures.has_value());
  REQUIRE(registry.freeze().has_value());

  mem::tracking_resource tracked("tracked", *events, &registry);
  mem::diagnostics diag(registry, 1);
  diag.add(tracked);
  auto a = tracked.try_allocate(1000, 8, *events);
  REQUIRE(a.has_value());
  REQUIRE(diag.report_external(*projection, 1U << 20U).has_value());

  const auto tracked_live = [](const mem::memory_report &r) {
    std::uint64_t sum = 0;
    for (const mem::tag_stats &t : r.tags) {
      sum += t.live_bytes;
      CHECK_FALSE(t.tag.starts_with("fabric."));
      CHECK_FALSE(t.tag.starts_with("gpu."));
    }
    return sum;
  };
  mem::memory_report r = diag.make_report(1);
  CHECK(tracked_live(r) == 1000);
  REQUIRE(r.external.size() == 2);
  CHECK(r.external[0].tag == "fabric.projection");
  CHECK(r.external[0].bytes == (1U << 20U));
  CHECK(r.external[1].tag == "gpu.textures");
  CHECK(r.external[1].bytes == 0);

  // report_external sets (not adds) the bytes
  REQUIRE(diag.report_external(*textures, 4096).has_value());
  REQUIRE(diag.report_external(*projection, 512).has_value());
  r = diag.make_report(2);
  CHECK(tracked_live(r) == 1000);
  CHECK(r.external[0].bytes == 512);
  CHECK(r.external[1].bytes == 4096);

  // a tracked block carrying an external tag is not counted as tracked bytes of the report
  auto stray = tracked.try_allocate(64, 8, *projection);
  REQUIRE(stray.has_value());
  r = diag.make_report(3);
  CHECK(tracked_live(r) == 1000);
  CHECK(r.external[0].bytes == 512);
  tracked.release(*stray, 64, 8);

  // only external tags are accepted
  const auto not_external = diag.report_external(*events, 1);
  REQUIRE_FALSE(not_external.has_value());
  CHECK(not_external.error().code == sl::errc::invalid_argument);
  const auto unknown = diag.report_external(mem::memory_tag{200}, 1);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == sl::errc::invalid_argument);
  const auto untagged = diag.report_external(mem::memory_tag{}, 1);
  REQUIRE_FALSE(untagged.has_value());
  CHECK(untagged.error().code == sl::errc::invalid_argument);
  tracked.release(*a, 1000, 8);
}
