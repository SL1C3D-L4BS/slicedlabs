// RT-INV-STORE-05 (SL-ARCH-003 ARCH3-R110, `failpoints` RFP0, ADR-0025): a structural change
// reserves everything it needs before it moves anything, so RFP0 (alloc.fail) at any allocation
// index of a structural-change batch fails that one call with out_of_memory and leaves the world
// hash and the oracle comparison unchanged, the rest of the batch still runs, and after the world
// is destroyed nothing is leaked (tracking_resource live bytes 0, gen_tracked balanced). The same
// holds for restore_from. A dry run counts the RFP0 visits of the batch and records which operation
// makes each one, so only that operation is hashed around. Every index is armed; the sweep step is
// 1 / SL_RUNTIME_STRESS_SCALE (0.1 under the sanitizers gives every tenth index), floored at 1.
// Skipped when the failpoints are compiled out (Release).

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::uint32_t no_tracked = 0xFFU & ~(1U << 5U);

std::uint64_t sweep_step() {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  if (scale <= 0.0 || scale >= 1.0) {
    return 1;
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(1.0 / scale));
}

// A world with its own tracking resource and oracle; destroy() ends the world first and reports
// whether anything leaked.
struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  void destroy() {
    w.reset();
    CHECK(memory.totals().live_bytes == 0);
    CHECK(oracle->counter().live() == 0);
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"store.sweep"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

std::vector<rtt::world_op> batch(std::uint64_t seed, int count, std::uint32_t mask = 0xFFU) {
  rtt::op_generator gen(seed, mask);
  std::vector<rtt::world_op> ops;
  ops.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    ops.push_back(gen.next());
  }
  return ops;
}

// Applies ops[first, last); returns how many the world accepted.
int run(fixture &f, const std::vector<rtt::world_op> &ops, std::size_t first = 0,
        std::size_t last = SIZE_MAX) {
  const std::size_t end = std::min(last, ops.size());
  const std::size_t begin = std::min(first, end);
  const std::span<const rtt::world_op> range = std::span(ops).subspan(begin, end - begin);
  return static_cast<int>(std::ranges::count_if(range, [&f](const rtt::world_op &op) {
    return rtt::apply(op, *f.w, *f.oracle).has_value();
  }));
}

} // namespace

TEST_CASE("RT-INV-STORE-05: RFP0 at every allocation of a structural batch changes nothing") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
    return;
  }
  const std::vector<rtt::world_op> ops = batch(0x088F, 300);

  // Dry run: the cumulative RFP0 visits after each operation.
  std::vector<std::uint64_t> visits_after;
  {
    fixture f;
    const std::uint64_t start = rt::visits(rt::failpoint::rfp0);
    for (const rtt::world_op &op : ops) {
      const auto r = rtt::apply(op, *f.w, *f.oracle);
      REQUIRE((r.has_value() || r.error().code != sl::errc::out_of_memory));
      visits_after.push_back(rt::visits(rt::failpoint::rfp0) - start);
    }
    f.destroy();
  }
  const std::uint64_t total = visits_after.back();
  REQUIRE(total > 20); // slabs, archetypes, chunk lists, transitions, location pages
  MESSAGE("RFP0 visits in the batch: " << total << ", step " << sweep_step());

  for (std::uint64_t k = 1; k <= total; k += sweep_step()) {
    INFO("allocation index " << k);
    const auto hit = std::ranges::lower_bound(visits_after, k); // the op making visit k
    const auto at = static_cast<std::size_t>(hit - visits_after.begin());
    fixture f;
    REQUIRE(rtt::arm(rt::failpoint::rfp0, k).has_value());
    CHECK(run(f, ops, 0, at) >= 0);
    const auto before = rtt::hash_world(*f.w, f.ids);
    const auto version = f.w->version();
    const auto failed = rtt::apply(ops[at], *f.w, *f.oracle);
    rtt::disarm(rt::failpoint::rfp0);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::out_of_memory);
    CHECK(rtt::hash_world(*f.w, f.ids) == before);
    CHECK(f.w->version() == version);
    const auto diff = f.oracle->compare(*f.w);
    CHECK(diff.empty());
    // The world goes on: the rest of the batch runs and still matches the oracle.
    CHECK(run(f, ops, at + 1) >= 0); // none left when the last op failed
    CHECK(f.oracle->compare(*f.w).empty());
    f.destroy();
  }
}

TEST_CASE("RT-INV-STORE-05: RFP0 at every allocation of restore_from changes nothing") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
    return;
  }
  fixture source;
  CHECK(run(source, batch(0x50C, 400, no_tracked)) > 0);
  const ecs::world_image image = source.w->capture_image();
  const auto source_hash = rtt::hash_world(*source.w, source.ids);
  const std::vector<rtt::world_op> prior = batch(0x7A6, 120, no_tracked);

  std::uint64_t total = 0;
  {
    fixture f;
    CHECK(run(f, prior) > 0);
    const std::uint64_t start = rt::visits(rt::failpoint::rfp0);
    REQUIRE(f.w->restore_from(image).has_value());
    total = rt::visits(rt::failpoint::rfp0) - start;
    CHECK(rtt::hash_world(*f.w, f.ids) == source_hash);
    f.w.reset();
    CHECK(f.memory.totals().live_bytes == 0);
  }
  REQUIRE(total > 5);
  MESSAGE("RFP0 visits in restore_from: " << total);

  for (std::uint64_t k = 1; k <= total; k += sweep_step()) {
    INFO("allocation index " << k);
    fixture f;
    CHECK(run(f, prior) > 0);
    const auto before = rtt::hash_world(*f.w, f.ids);
    REQUIRE(rtt::arm(rt::failpoint::rfp0, k).has_value());
    const auto failed = f.w->restore_from(image);
    rtt::disarm(rt::failpoint::rfp0);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::out_of_memory);
    CHECK(rtt::hash_world(*f.w, f.ids) == before);
    CHECK(f.oracle->compare(*f.w).empty());
    REQUIRE(f.w->restore_from(image).has_value()); // the world is still usable
    CHECK(rtt::hash_world(*f.w, f.ids) == source_hash);
    f.w.reset();
    CHECK(f.memory.totals().live_bytes == 0);
  }
  source.destroy();
}
