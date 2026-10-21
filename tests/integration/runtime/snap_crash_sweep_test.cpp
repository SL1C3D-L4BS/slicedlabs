// RT-INV-SNAP-04 (SL-ARCH-003 ARCH3-R147, ADR-0026, RFP8): a crash after every storage primitive
// of snapshot::save, under each faulty-vfs crash policy, leaves the old or the new snapshot
// loadable. For each policy (keep_unsynced, drop_unsynced, tear_last_append at several tear
// points) and each k from 0 to the primitive count of a clean save: the old snapshot is saved,
// primitive k of the new save and every later one fail, the vfs crashes, and load returns the old
// or the new bytes exactly; a save that returned success is the new one. The loaded snapshot
// restores into a fresh world with its recorded state hash, and a later save succeeds. With the
// failpoints compiled in, RFP8 fails save before its first primitive and the old file stays.
// Rounds scale with SL_RUNTIME_STRESS_SCALE (0.1 under the sanitizers, floored at 1).

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"
#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;
namespace ft = sl::fabric::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
// position, velocity and health
constexpr std::uint32_t moved_kinds = (1U << 0U) | (1U << 1U) | (1U << 7U);
constexpr const char *file_name = "world.slrs";

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

/// Three reflected world_gen components; the other kinds are left out of the operations.
struct fixture {
  fixture() {
    rt::type_builder<rtt::gen_position> position("gen.position");
    SL_RT_FIELD(position, rtt::gen_position, x, kept);
    SL_RT_FIELD(position, rtt::gen_position, y, kept);
    SL_RT_FIELD(position, rtt::gen_position, z, kept);
    rt::type_builder<rtt::gen_velocity> velocity("gen.velocity");
    SL_RT_FIELD(velocity, rtt::gen_velocity, dx, kept);
    SL_RT_FIELD(velocity, rtt::gen_velocity, dy, kept);
    rt::type_builder<rtt::gen_health> health("gen.health");
    SL_RT_FIELD(health, rtt::gen_health, hp, kept);
    SL_RT_FIELD(health, rtt::gen_health, armor, kept);
    const auto add = [this](const rt::type_info &info) {
      auto t = types.add(info);
      REQUIRE(t.has_value());
      return *t;
    };
    const std::array<sl::result<ecs::component_id>, rtt::gen_component_count> got = {
        registry.add<rtt::gen_position>("gen.position", add(position.build())),
        registry.add<rtt::gen_velocity>("gen.velocity", add(velocity.build())),
        registry.add<rtt::gen_padded>("gen.padded"),
        registry.add<rtt::gen_large>("gen.large"),
        registry.add<rtt::gen_tag>("gen.tag"),
        registry.add<rtt::gen_tracked>("gen.tracked"),
        registry.add<rtt::gen_aligned>("gen.aligned"),
        registry.add<rtt::gen_health>("gen.health", add(health.build()))};
    for (std::size_t k = 0; k < got.size(); ++k) {
      REQUIRE(got.at(k).has_value());
      ids.ids.at(k) = *got.at(k);
    }
    REQUIRE(types.freeze().has_value());
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    w = make_world();
  }

  [[nodiscard]] std::unique_ptr<ecs::world> make_world() {
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    return std::move(*made);
  }

  /// `ops` more seeded operations on positions, velocities and health, then a snapshot.
  [[nodiscard]] rt::snapshot advance(std::uint64_t seed, int ops) {
    rtt::op_generator gen(seed, moved_kinds);
    for (int i = 0; i < ops; ++i) {
      [[maybe_unused]] const auto applied = rtt::apply(gen.next(), *w, *oracle);
    }
    ++tick;
    rt::snapshot_meta meta;
    meta.frame = tick;
    meta.tick = tick;
    auto s = rt::take(*w, registry, types, {}, meta);
    REQUIRE(s.has_value());
    return *s;
  }

  rt::type_registry types;
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"snap.crash"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world
  std::unique_ptr<ecs::world> w;
  std::uint64_t tick = 0;
};

/// The primitives a clean save of `s` takes on a fresh vfs.
std::uint64_t clean_primitives(const rt::snapshot &s) {
  ft::faulty_vfs fs;
  const rt::file_access files(fs, "/");
  REQUIRE(rt::save(files, file_name, s).has_value());
  return fs.primitives(); // a fresh vfs counts from zero
}

struct sweep_case {
  ft::crash_policy policy;
  std::uint64_t tear_at;
};

/// One crash point: old saved, new save failing at primitive k, crash, load.
void crash_at(fixture &f, const rt::snapshot &old_s, const rt::snapshot &new_s, std::uint64_t k,
              const sweep_case &c) {
  CAPTURE(k);
  CAPTURE(static_cast<int>(c.policy));
  CAPTURE(c.tear_at);
  ft::faulty_vfs fs;
  const rt::file_access files(fs, "/");
  REQUIRE(rt::save(files, file_name, old_s).has_value());
  fs.fail_after(k);
  const bool saved = rt::save(files, file_name, new_s).has_value();
  REQUIRE(fs.crash(c.policy, c.tear_at).has_value());
  fs.disarm();

  auto loaded = rt::load(files, file_name);
  REQUIRE(loaded.has_value());
  const bool is_old = std::ranges::equal(loaded->bytes(), old_s.bytes());
  const bool is_new = std::ranges::equal(loaded->bytes(), new_s.bytes());
  CHECK((is_old || is_new));
  if (saved) {
    CHECK(is_new); // a save that returned success is durable
  }
  std::unique_ptr<ecs::world> target = f.make_world();
  REQUIRE(rt::restore(*target, f.registry, f.types, {}, *loaded, {}).has_value());
  auto h = rt::state_hash(*target, f.registry, f.types, {});
  REQUIRE(h.has_value());
  CHECK(*h == loaded->state_hash());

  // the store works again: a later save lands
  REQUIRE(rt::save(files, file_name, new_s).has_value());
  auto again = rt::load(files, file_name);
  REQUIRE(again.has_value());
  CHECK(std::ranges::equal(again->bytes(), new_s.bytes()));
}

} // namespace

TEST_CASE("RT-INV-SNAP-04: a crash after every primitive of save leaves the old or the new "
          "snapshot") {
  fixture f;
  rt::snapshot old_s = f.advance(0x5EED0098'0400ULL, 600);
  const std::uint32_t rounds = scaled(4);
  std::uint64_t points = 0;
  for (std::uint32_t round = 0; round < rounds; ++round) {
    CAPTURE(round);
    rt::snapshot new_s = f.advance(0x5EED0098'0401ULL + round, 150);
    REQUIRE_FALSE(std::ranges::equal(old_s.bytes(), new_s.bytes()));
    const std::uint64_t primitives = clean_primitives(new_s);
    REQUIRE(primitives >= 4); // open, append, fsync, rename at least
    const std::uint64_t size = new_s.bytes().size();
    using enum ft::crash_policy;
    const std::array<sweep_case, 5> cases{{{.policy = keep_unsynced, .tear_at = 0},
                                           {.policy = drop_unsynced, .tear_at = 0},
                                           {.policy = tear_last_append, .tear_at = 0},
                                           {.policy = tear_last_append, .tear_at = size / 2},
                                           {.policy = tear_last_append, .tear_at = size - 1}}};
    for (const sweep_case &c : cases) {
      for (std::uint64_t k = 0; k <= primitives; ++k) {
        crash_at(f, old_s, new_s, k, c);
        ++points;
      }
    }
    old_s = std::move(new_s);
  }
  CHECK(points >= rounds * 5 * 5);
}

TEST_CASE("RT-INV-SNAP-04: RFP8 fails save before its first primitive and the old file stays") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out: RFP8 is not reachable in this build");
    return;
  }
  fixture f;
  const rt::snapshot old_s = f.advance(0x5EED0098'0480ULL, 300);
  const rt::snapshot new_s = f.advance(0x5EED0098'0481ULL, 100);
  ft::faulty_vfs fs;
  const rt::file_access files(fs, "/");
  REQUIRE(rt::save(files, file_name, old_s).has_value());
  const std::uint64_t before = fs.primitives();
  {
    const rtt::scoped_arm armed(rt::failpoint::rfp8, 1);
    REQUIRE(armed.status().has_value());
    auto failed = rt::save(files, file_name, new_s);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::io_error);
  }
  CHECK(fs.primitives() == before);
  REQUIRE(fs.crash(ft::crash_policy::drop_unsynced).has_value());
  auto loaded = rt::load(files, file_name);
  REQUIRE(loaded.has_value());
  CHECK(std::ranges::equal(loaded->bytes(), old_s.bytes()));
  REQUIRE(rt::save(files, file_name, new_s).has_value()); // disarmed: the next save lands
}
