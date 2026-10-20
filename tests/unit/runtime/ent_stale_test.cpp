// RT-INV-ENT-01 (SL-ARCH-003 ARCH3-R103, ADR-0025): a destroyed handle is never alive again across
// 1e6 seeded create and destroy cycles (scaled by SL_RUNTIME_STRESS_SCALE, 0.1 under the
// sanitizers, floored at 1). The live population follows seeded targets between 0 and
// max_entities, so both the fresh-index path and the FIFO-reuse path run; every create must
// return a generation above the last destroyed one of its index, recently destroyed handles are
// probed on every step and every last destroyed handle is probed at the end. is_alive is
// any-thread while nothing mutates: four reader threads probe a fixed population.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;

constexpr ecs::entity ent(std::uint32_t index, std::uint32_t generation) noexcept {
  return ecs::entity{.index = index, .generation = generation};
}

constexpr double base_cycles = 1e6;

std::uint64_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(base * scale));
}

// splitmix64: the same sequence on every platform (no std distribution).
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

} // namespace

TEST_CASE("RT-INV-ENT-01: a destroyed handle is never alive again across seeded cycles") {
  constexpr std::uint32_t max = 8192;
  constexpr std::size_t ring_size = 1024;
  constexpr std::uint64_t retarget_every = 8192;
  mem::tracking_resource r("ent.stale");
  ecs::entity_allocator a(max, r);
  const std::uint64_t cycles = scaled(base_cycles);
  splitmix64 rng{0x5EED0910001ULL};

  std::vector<ecs::entity> live;
  live.reserve(max);
  std::vector<std::uint32_t> last_dead(max, 0); // the last destroyed generation per index
  std::vector<ecs::entity> recent(ring_size);   // recently destroyed handles
  std::size_t recent_next = 0;
  std::uint64_t created = 0;
  std::uint64_t destroyed = 0;
  std::uint64_t violations = 0;
  std::uint64_t failures = 0;
  std::uint64_t reused = 0;
  std::uint32_t target = max / 2;

  for (std::uint64_t step = 0; created < cycles || destroyed < cycles; ++step) {
    if (step % retarget_every == 0) {
      target = rng.below(max);
    }
    const bool want_create = live.size() < target ? rng.below(4) != 0 : rng.below(4) == 0;
    const bool do_create = created < cycles && live.size() < max && (want_create || live.empty());
    if (do_create) {
      auto made = a.create();
      if (!made) {
        ++failures;
        break;
      }
      const ecs::entity e = *made;
      if (e.generation <= last_dead[e.index] || !a.is_alive(e)) {
        ++violations;
      }
      if (last_dead[e.index] != 0) {
        ++reused;
      }
      live.push_back(e);
      ++created;
    } else if (!live.empty()) {
      const std::size_t k = rng.below(live.size());
      const ecs::entity e = live[k];
      if (!a.destroy(e)) {
        ++failures;
      }
      if (a.is_alive(e)) {
        ++violations;
      }
      if (destroyed % 64 == 0 && a.destroy(e).has_value()) {
        ++violations; // a second destroy of the same handle must be stale
      }
      last_dead[e.index] = e.generation;
      recent[recent_next % ring_size] = e;
      ++recent_next;
      live[k] = live.back();
      live.pop_back();
      ++destroyed;
    }
    if (recent_next != 0) {
      const ecs::entity probe = recent[rng.below(std::min<std::size_t>(recent_next, ring_size))];
      if (a.is_alive(probe)) {
        ++violations;
      }
    }
  }

  CHECK(violations == 0);
  CHECK(failures == 0);
  CHECK(created == cycles);
  CHECK(a.alive() == live.size());
  if (cycles >= 100000) {
    CHECK(reused > 0); // the FIFO path ran
  }
  std::uint64_t still_alive = 0;
  for (std::uint32_t i = 0; i < max; ++i) {
    if (last_dead[i] != 0 && a.is_alive(ent(i, last_dead[i]))) {
      ++still_alive;
    }
  }
  CHECK(still_alive == 0);
  for (const ecs::entity e : live) {
    REQUIRE(a.destroy(e).has_value());
    CHECK_FALSE(a.is_alive(e));
  }
  CHECK(a.alive() == 0);
}

TEST_CASE("RT-INV-ENT-01: the null handle and never-issued handles are never alive") {
  mem::tracking_resource r("ent.null");
  ecs::entity_allocator a(16, r);
  CHECK_FALSE(a.is_alive(ecs::null_entity));
  auto first = a.create();
  REQUIRE(first.has_value());
  CHECK(*first == ent(0, ecs::entity_generation_first));
  CHECK_FALSE(first->is_null());
  CHECK_FALSE(a.is_alive(ecs::null_entity));
  CHECK_FALSE(a.is_alive(ent(0, 0)));
  CHECK_FALSE(a.is_alive(ent(0, 2)));
  CHECK_FALSE(a.is_alive(ent(1, 1))); // index not handed out yet
  CHECK_FALSE(a.is_alive(ent(4096, 1)));
  CHECK(a.destroy(ecs::null_entity).error().code == sl::errc::stale_entity);
  REQUIRE(a.destroy(*first).has_value());
  CHECK_FALSE(a.is_alive(*first));
  CHECK(a.destroy(*first).error().code == sl::errc::stale_entity);
}

TEST_CASE("RT-INV-ENT-01: is_alive is any-thread while nothing mutates") {
  constexpr std::uint32_t count = 6000;
  constexpr std::size_t readers = 4;
  mem::tracking_resource r("ent.readers");
  ecs::entity_allocator a(count, r);
  std::vector<ecs::entity> alive_handles;
  std::vector<ecs::entity> dead_handles;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    alive_handles.push_back(*e);
  }
  for (std::uint32_t i = 0; i < count; i += 3) {
    REQUIRE(a.destroy(alive_handles[i]).has_value());
    dead_handles.push_back(alive_handles[i]);
  }
  std::erase_if(alive_handles, [&](ecs::entity e) { return e.index % 3 == 0; });
  std::atomic<std::uint64_t> wrong{0};
  {
    std::vector<std::jthread> threads;
    threads.reserve(readers);
    for (std::size_t t = 0; t < readers; ++t) {
      threads.emplace_back([&] {
        std::uint64_t local = 0;
        for (int round = 0; round < 8; ++round) {
          for (const ecs::entity e : alive_handles) {
            local += a.is_alive(e) && a.check(e).has_value() ? 0U : 1U;
          }
          for (const ecs::entity e : dead_handles) {
            local += a.is_alive(e) ? 1U : 0U;
          }
        }
        wrong += local;
      });
    }
  }
  CHECK(wrong.load() == 0);
}
