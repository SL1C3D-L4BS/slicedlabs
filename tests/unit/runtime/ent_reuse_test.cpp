// RT-INV-ENT-03 (SL-ARCH-003 ARCH3-R102, `determinism` entity allocator, ADR-0025, ADR-0026):
// freed indices are reused in FIFO order of destruction only while at least 1024 are free (or
// when max_entities is reached), and the index sequence is identical across runs. A hand-checked
// scenario pins the threshold edges; a seeded history is compared with a reference model of the
// rule on every create; two allocators fed the same history hand out the same handles; and an
// allocator restored from a capture continues exactly as the original.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;

constexpr ecs::entity ent(std::uint32_t index, std::uint32_t generation) noexcept {
  return ecs::entity{.index = index, .generation = generation};
}

constexpr std::uint32_t threshold = ecs::entity_reuse_fifo_threshold;

std::uint64_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(base * scale));
}

struct splitmix64 {
  std::uint64_t state = 0;
  std::uint64_t next() noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint32_t below(std::uint64_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }
};

// The rule of ARCH3-R102 written out independently: the index the next create must return.
struct reference_model {
  std::uint32_t max = 0;
  std::uint32_t next = 0;
  std::deque<std::uint32_t> fifo;

  [[nodiscard]] bool can_create() const { return next < max || !fifo.empty(); }
  std::uint32_t create() {
    if (fifo.size() >= threshold || next == max) {
      const std::uint32_t i = fifo.front();
      fifo.pop_front();
      return i;
    }
    return next++;
  }
  void destroy(std::uint32_t index) { fifo.push_back(index); }
};

// Runs a seeded history of `ops` operations and returns the bits of every handle created.
std::vector<std::uint64_t> history(ecs::entity_allocator &a, std::uint64_t seed, std::uint64_t ops,
                                   std::vector<ecs::entity> &live) {
  splitmix64 rng{seed};
  std::vector<std::uint64_t> out;
  for (std::uint64_t op = 0; op < ops; ++op) {
    const bool grow = (op / 4096) % 2 == 0;
    const bool create = live.empty() || (grow ? rng.below(8) < 5 : rng.below(8) < 3);
    if (create) {
      auto e = a.create();
      if (!e) {
        continue; // capacity: part of the history
      }
      live.push_back(*e);
      out.push_back(e->bits());
    } else {
      const std::size_t k = rng.below(live.size());
      REQUIRE(a.destroy(live[k]).has_value());
      live[k] = live.back();
      live.pop_back();
    }
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-ENT-03: freed indices are reused FIFO only while at least 1024 are free") {
  mem::tracking_resource r("ent.reuse");
  ecs::entity_allocator a(1U << 16U, r);
  std::vector<ecs::entity> made;
  for (std::uint32_t i = 0; i < 3000; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    REQUIRE(e->index == i);
    made.push_back(*e);
  }
  // Destroy indices 0..1022 in a scrambled order (7919 is prime to 1023): below the threshold,
  // create takes fresh indices.
  std::vector<std::uint32_t> order;
  order.reserve(threshold - 1);
  for (std::uint32_t i = 0; i < threshold - 1; ++i) {
    order.push_back(((i * 7919U) + 5U) % (threshold - 1));
  }
  for (const std::uint32_t i : order) {
    REQUIRE(a.destroy(made[i]).has_value());
  }
  CHECK(a.free_count() == threshold - 1);
  auto fresh = a.create();
  REQUIRE(fresh.has_value());
  CHECK(*fresh == ent(3000, 1));
  // One more destroy: 1024 free, so create reuses the oldest freed index with generation 2.
  REQUIRE(a.destroy(made[2999]).has_value());
  CHECK(a.free_count() == threshold);
  auto reused = a.create();
  REQUIRE(reused.has_value());
  CHECK(*reused == ent(order[0], 2));
  CHECK(a.free_count() == threshold - 1);
  // Back below the threshold: fresh again.
  auto fresh2 = a.create();
  REQUIRE(fresh2.has_value());
  CHECK(*fresh2 == ent(3001, 1));
  // Two more destroys: 1025 free; two reuses in FIFO order, then fresh.
  REQUIRE(a.destroy(made[2998]).has_value());
  REQUIRE(a.destroy(made[2997]).has_value());
  auto r1 = a.create();
  auto r2 = a.create();
  auto f3 = a.create();
  REQUIRE((r1.has_value() && r2.has_value() && f3.has_value()));
  CHECK(r1->index == order[1]);
  CHECK(r2->index == order[2]);
  CHECK(*f3 == ent(3002, 1));
}

TEST_CASE("RT-INV-ENT-03: at max_entities the oldest freed index is reused below the threshold") {
  mem::tracking_resource r("ent.reuse.max");
  ecs::entity_allocator a(10, r);
  std::vector<ecs::entity> made;
  for (std::uint32_t i = 0; i < 10; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    made.push_back(*e);
  }
  for (const std::uint32_t i : {3U, 7U, 1U}) {
    REQUIRE(a.destroy(made[i]).has_value());
  }
  for (const std::uint32_t i : {3U, 7U, 1U}) {
    auto e = a.create();
    REQUIRE(e.has_value());
    CHECK(*e == ent(i, 2));
  }
  CHECK(a.create().error().code == sl::errc::entity_capacity);
}

TEST_CASE("RT-INV-ENT-03: every create matches the reference model of the reuse rule") {
  constexpr std::uint32_t max = 6000;
  mem::tracking_resource r("ent.reuse.model");
  ecs::entity_allocator a(max, r);
  reference_model model{.max = max, .next = 0, .fifo = {}};
  splitmix64 rng{0x5EED0910003ULL};
  std::vector<ecs::entity> live;
  const std::uint64_t ops = scaled(4e5);
  std::uint64_t mismatches = 0;
  std::uint64_t reuses = 0;
  for (std::uint64_t op = 0; op < ops; ++op) {
    const bool grow = (op / 5000) % 2 == 0;
    const bool create = live.empty() || (grow ? rng.below(8) < 5 : rng.below(8) < 3);
    if (create) {
      if (!model.can_create()) {
        mismatches += a.create().has_value() ? 1U : 0U;
        continue;
      }
      const std::uint32_t expected = model.create();
      auto e = a.create();
      if (!e || e->index != expected) {
        ++mismatches;
        continue;
      }
      reuses += e->generation > 1 ? 1U : 0U;
      live.push_back(*e);
    } else {
      const std::size_t k = rng.below(live.size());
      if (!a.destroy(live[k])) {
        ++mismatches;
      }
      model.destroy(live[k].index);
      live[k] = live.back();
      live.pop_back();
    }
  }
  CHECK(mismatches == 0);
  CHECK(a.free_count() == model.fifo.size());
  CHECK(a.next_index() == model.next);
  if (ops >= 100000) {
    CHECK(reuses > 0);
  }
  const ecs::entity_allocator::state s = a.capture();
  CHECK(std::ranges::equal(s.free_fifo, model.fifo));
}

TEST_CASE("RT-INV-ENT-03: the handle sequence is identical across runs and after restore") {
  constexpr std::uint32_t max = 5000;
  constexpr std::uint64_t seed = 0x5EED0910030ULL;
  const std::uint64_t ops = scaled(2e5);
  mem::tracking_resource r1("ent.run1");
  mem::tracking_resource r2("ent.run2");
  ecs::entity_allocator a1(max, r1);
  ecs::entity_allocator a2(max, r2);
  std::vector<ecs::entity> live1;
  std::vector<ecs::entity> live2;
  const std::vector<std::uint64_t> run1 = history(a1, seed, ops, live1);
  const std::vector<std::uint64_t> run2 = history(a2, seed, ops, live2);
  CHECK(run1.size() > ops / 4);
  CHECK(run1 == run2);
  CHECK(a1.capture() == a2.capture());

  // Restore a capture into a third allocator: continuing it equals continuing the original.
  mem::tracking_resource r3("ent.run3");
  ecs::entity_allocator a3(max, r3);
  REQUIRE(a3.restore(a1.capture()).has_value());
  CHECK(a3.alive() == a1.alive());
  CHECK(a3.free_count() == a1.free_count());
  std::vector<ecs::entity> live3 = live1;
  const std::vector<std::uint64_t> cont1 = history(a1, seed + 1, ops / 2, live1);
  const std::vector<std::uint64_t> cont3 = history(a3, seed + 1, ops / 2, live3);
  CHECK(cont1 == cont3);
  CHECK(a1.capture() == a3.capture());

  // A fixed prefix: the first handles of the history never change between builds or runs.
  mem::tracking_resource r4("ent.run4");
  ecs::entity_allocator a4(max, r4);
  std::vector<ecs::entity> live4;
  const std::vector<std::uint64_t> prefix = history(a4, seed, 8, live4);
  REQUIRE_FALSE(prefix.empty());
  CHECK(ecs::entity::from_bits(prefix.front()) == ent(0, 1));
  CHECK(std::equal(prefix.begin(), prefix.end(), run1.begin()));
}
