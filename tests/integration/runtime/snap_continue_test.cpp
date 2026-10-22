// RT-INV-SNAP-02 (SL-ARCH-003 ARCH3-R145, ADR-0026): 300 ticks after a restore equal 300 ticks of
// the original, with the state hash compared every tick. A tick is a batch of world_gen
// operations whose seed comes from a seeded driver rng_stream; the driver is the snapshot's rng
// contributor, so the restored world draws the same seeds. The original world A runs a prefix,
// is snapshotted at the tick boundary and restored into a fresh world B; then both run 300 ticks
// and agree on the state hash and on world_gen's hash after each, and on the oracle at the end.
// A runtime recovery continues the same way (WO-109): the restore rewinds the logical tick and
// every system's last run, so after a fault the run hashes equal to a fault-free run at the same
// logical tick, a changed<T> reader included. Operations per tick scale with
// SL_RUNTIME_STRESS_SCALE (0.1 under the sanitizers, floored at 1).

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
// every kind but gen_large (2 KiB a value, hashed four times a tick) and gen_tracked
constexpr std::uint32_t ticked_kinds = 0xFFU & ~(1U << 3U) & ~(1U << 5U);
constexpr int continued_ticks = 300;

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

/// The world_gen components with reflection (gen_tracked without), in world_gen's kind order.
void register_reflected(rt::type_registry &types, ecs::component_registry &registry,
                        rtt::gen_ids &ids) {
  rt::type_builder<rtt::gen_position> position("gen.position");
  SL_RT_FIELD(position, rtt::gen_position, x, kept);
  SL_RT_FIELD(position, rtt::gen_position, y, kept);
  SL_RT_FIELD(position, rtt::gen_position, z, kept);
  rt::type_builder<rtt::gen_velocity> velocity("gen.velocity");
  SL_RT_FIELD(velocity, rtt::gen_velocity, dx, kept);
  SL_RT_FIELD(velocity, rtt::gen_velocity, dy, kept);
  rt::type_builder<rtt::gen_padded> padded("gen.padded");
  SL_RT_FIELD(padded, rtt::gen_padded, a, kept);
  SL_RT_FIELD(padded, rtt::gen_padded, b, kept);
  SL_RT_FIELD(padded, rtt::gen_padded, c, kept);
  rt::type_builder<rtt::gen_large> large("gen.large");
  large.bytes("words", 0, sizeof(rtt::gen_large), kept);
  const rt::type_builder<rtt::gen_tag> tag("gen.tag");
  rt::type_builder<rtt::gen_aligned> aligned("gen.aligned");
  SL_RT_FIELD(aligned, rtt::gen_aligned, value, kept);
  rt::type_builder<rtt::gen_health> health("gen.health");
  SL_RT_FIELD(health, rtt::gen_health, hp, kept);
  SL_RT_FIELD(health, rtt::gen_health, armor, kept);
  const auto add = [&types](const rt::type_info &info) {
    auto t = types.add(info);
    REQUIRE(t.has_value());
    return *t;
  };
  const std::array<sl::result<ecs::component_id>, rtt::gen_component_count> got = {
      registry.add<rtt::gen_position>("gen.position", add(position.build())),
      registry.add<rtt::gen_velocity>("gen.velocity", add(velocity.build())),
      registry.add<rtt::gen_padded>("gen.padded", add(padded.build())),
      registry.add<rtt::gen_large>("gen.large", add(large.build())),
      registry.add<rtt::gen_tag>("gen.tag", add(tag.build())),
      registry.add<rtt::gen_tracked>("gen.tracked"),
      registry.add<rtt::gen_aligned>("gen.aligned", add(aligned.build())),
      registry.add<rtt::gen_health>("gen.health", add(health.build()))};
  for (std::size_t k = 0; k < got.size(); ++k) {
    REQUIRE(got.at(k).has_value());
    ids.ids.at(k) = *got.at(k);
  }
}

struct fixture {
  fixture() {
    register_reflected(types, registry, ids);
    REQUIRE(types.freeze().has_value());
    REQUIRE(registry.freeze().has_value());
  }

  [[nodiscard]] std::unique_ptr<ecs::world> make_world() {
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    return std::move(*made);
  }

  rt::type_registry types;
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"snap.continue"};
};

/// One simulated world: the world, its oracle, and the driver stream behind its rng contributor.
struct sim {
  sim(fixture &f, std::uint64_t seed)
      : oracle(std::make_unique<rtt::world_oracle>(f.ids)), driver(seed, 98),
        rng(std::span<det::rng_stream *const>(streams)), w(f.make_world()) {}

  /// One tick: `ops` operations seeded by the next driver draw.
  void tick(std::uint32_t ops) {
    rtt::op_generator gen(driver.next_u64(), ticked_kinds);
    for (std::uint32_t i = 0; i < ops; ++i) {
      [[maybe_unused]] const auto applied = rtt::apply(gen.next(), *w, *oracle);
    }
  }

  [[nodiscard]] sl::fabric::digest hash(const fixture &f) const {
    const std::array<const det::snapshot_contributor *, 1> contributors{&rng};
    auto h = rt::state_hash(*w, f.registry, f.types, contributors);
    REQUIRE(h.has_value());
    return *h;
  }

  [[nodiscard]] rt::hash_breakdown parts(const fixture &f) const {
    const std::array<const det::snapshot_contributor *, 1> contributors{&rng};
    auto b = rt::breakdown(*w, f.registry, f.types, contributors);
    REQUIRE(b.has_value());
    return *b;
  }

  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world
  det::rng_stream driver;
  std::array<det::rng_stream *, 1> streams{&driver};
  det::rng_contributor rng;
  std::unique_ptr<ecs::world> w;
};

sl::fabric::digest abi_digest() {
  sl::fabric::digest d;
  d.bytes.at(0) = 0x98;
  return d;
}

void check_continue(std::uint64_t seed, int prefix, std::uint32_t ops) {
  CAPTURE(seed);
  CAPTURE(prefix);
  fixture f;
  sim a(f, seed);
  for (int t = 0; t < prefix; ++t) {
    a.tick(ops);
  }
  rt::snapshot_meta meta;
  meta.frame = static_cast<std::uint64_t>(prefix) + 1;
  meta.tick = static_cast<std::uint64_t>(prefix);
  meta.runtime_abi_digest = abi_digest();
  const std::array<const det::snapshot_contributor *, 1> taken_from{&a.rng};
  auto s = rt::take(*a.w, f.registry, f.types, taken_from, meta);
  REQUIRE(s.has_value());

  // B's oracle comes from replaying the prefix on a scratch world; B itself from the snapshot
  sim b(f, seed ^ 0xFFFFU);
  {
    sim replay(f, seed);
    for (int t = 0; t < prefix; ++t) {
      replay.tick(ops);
    }
    b.oracle = std::move(replay.oracle);
  }
  const std::array<det::snapshot_contributor *, 1> restore_into{&b.rng};
  REQUIRE(rt::restore(*b.w, f.registry, f.types, restore_into, *s, abi_digest()).has_value());
  REQUIRE(b.driver == a.driver);
  REQUIRE(b.oracle->compare(*b.w).empty());
  REQUIRE(b.hash(f) == s->state_hash());

  int equal_ticks = 0;
  for (int t = 0; t < continued_ticks; ++t) {
    a.tick(ops);
    b.tick(ops);
    const bool same_state = a.hash(f) == b.hash(f);
    const bool same_values = rtt::hash_world(*a.w, f.ids) == rtt::hash_world(*b.w, f.ids);
    if (!same_state || !same_values) {
      CAPTURE(t);
      const std::optional<rt::hash_breakdown::part> first =
          rt::first_difference(a.parts(f), b.parts(f));
      CAPTURE(first.has_value() ? first->archetype : 0U);
      FAIL("the restored world diverged");
    }
    ++equal_ticks;
  }
  CHECK(equal_ticks == continued_ticks);
  CHECK(a.oracle->compare(*a.w).empty());
  CHECK(b.oracle->compare(*b.w).empty());
  CHECK(a.w->entities().capture() == b.w->entities().capture());
}

} // namespace

TEST_CASE("RT-INV-SNAP-02: 300 ticks after restore equal 300 ticks of the original") {
  const std::uint32_t ops = scaled(30);
  check_continue(0x5EED0098'0001ULL, 0, ops);
  check_continue(0x5EED0098'0002ULL, 37, ops);
  check_continue(0x5EED0098'0003ULL, 120, ops);
}

namespace {

namespace runtime_continue {

struct counter {
  std::uint64_t value = 0;
};

struct tally {
  std::uint64_t seen = 0;
};

struct writer_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

// Counts the rows changed since its last run: after a restore that kept a later last run it
// would see none.
struct reader_body {
  ecs::query<ecs::read<counter>, ecs::changed<counter>> changed;
  ecs::query<ecs::write<tally>> out;
  void operator()(ecs::system_context &ctx) noexcept {
    std::uint64_t n = 0;
    ctx.for_each(changed, [&n](ecs::entity /*e*/, const counter & /*c*/) noexcept { ++n; });
    ctx.for_each(out, [n](ecs::entity /*e*/, tally &t) noexcept { t.seen += n; });
  }
};

struct faulty_body {
  std::atomic<bool> *fail = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (fail->exchange(false)) {
      ctx.fail(sl::errc::job_failed);
    }
  }
};

struct rig {
  explicit rig(std::uint32_t id) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = 2;
    cfg.rate = {.num = 50, .den = 1};
    cfg.recovery = rt::recovery_policy::restore_snapshot;
    cfg.snapshot_every = 5;
    auto made = rt::runtime::create(plat.get(), cfg);
    REQUIRE(made.has_value());
    r = std::move(*made);
    rt::type_builder<counter> cb("cont.counter");
    SL_RT_FIELD(cb, counter, value, kept);
    auto ct = r->types().add(cb.build());
    REQUIRE(ct.has_value());
    REQUIRE(r->components().add<counter>("cont.counter", *ct).has_value());
    rt::type_builder<tally> tb("cont.tally");
    SL_RT_FIELD(tb, tally, seen, kept);
    auto tt = r->types().add(tb.build());
    REQUIRE(tt.has_value());
    REQUIRE(r->components().add<tally>("cont.tally", *tt).has_value());

    auto wq = ecs::query<ecs::write<counter>>::make(r->components());
    REQUIRE(wq.has_value());
    ecs::access_set wa;
    ecs::add_query_access(wa, wq->access());
    const ecs::component_access wneeded = wq->access();
    REQUIRE(r->add_system(ecs::system_desc{.name = "cont.writer",
                                           .ph = rt::phase::tick,
                                           .access = std::move(wa),
                                           .before = {},
                                           .after = {},
                                           .fn = writer_body{.q = std::move(*wq)},
                                           .chunked = false,
                                           .queries = {wneeded}})
                .has_value());
    auto cq = ecs::query<ecs::read<counter>, ecs::changed<counter>>::make(r->components());
    auto oq = ecs::query<ecs::write<tally>>::make(r->components());
    REQUIRE(cq.has_value());
    REQUIRE(oq.has_value());
    ecs::access_set ra;
    ecs::add_query_access(ra, cq->access());
    ecs::add_query_access(ra, oq->access());
    const ecs::component_access cneeded = cq->access();
    const ecs::component_access oneeded = oq->access();
    REQUIRE(r->add_system(ecs::system_desc{
                              .name = "cont.reader",
                              .ph = rt::phase::tick,
                              .access = std::move(ra),
                              .before = {},
                              .after = {},
                              .fn = reader_body{.changed = std::move(*cq), .out = std::move(*oq)},
                              .chunked = false,
                              .queries = {cneeded, oneeded}})
                .has_value());
    REQUIRE(r->add_system(ecs::system_desc{.name = "cont.faulty",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = faulty_body{.fail = &fail},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
    REQUIRE(r->start().has_value());
    for (std::uint32_t i = 0; i < 64; ++i) {
      REQUIRE(r->world().create_with(counter{.value = i}).has_value());
    }
    REQUIRE(r->world().create_with(tally{}).has_value());
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }

  [[nodiscard]] sl::fabric::digest hash() const {
    auto h = rt::state_hash(r->world(), r->components(), r->types(),
                            std::span<const det::snapshot_contributor *const>{});
    REQUIRE(h.has_value());
    return *h;
  }

  rtt::scripted_platform plat;
  std::atomic<bool> fail{false};
  std::unique_ptr<rt::runtime> r;
};

} // namespace runtime_continue

} // namespace

TEST_CASE(
    "RT-INV-SNAP-02: after a runtime recovery the run equals a fault-free run tick for tick") {
  constexpr rt::time::tick end_tick = 40;
  std::map<rt::time::tick, sl::fabric::digest> reference;
  {
    runtime_continue::rig g(901);
    while (g.now() < end_tick) {
      REQUIRE(g.frame().has_value());
      reference[g.now()] = g.hash();
    }
    REQUIRE(g.r->shutdown().has_value());
  }
  runtime_continue::rig g(902);
  while (g.now() < 13) {
    REQUIRE(g.frame().has_value());
    CHECK(g.hash() == reference.at(g.now()));
  }
  g.fail.store(true);
  REQUIRE(g.frame().has_value());
  REQUIRE(g.r->frame_faulted());
  CHECK(g.r->recoveries() == 1);
  CHECK(g.now() == 10); // the newest snapshot at or before the fault tick
  CHECK(g.hash() == reference.at(g.now()));
  while (g.now() < end_tick) {
    REQUIRE(g.frame().has_value());
    INFO("tick " << g.now());
    CHECK(g.hash() == reference.at(g.now()));
  }
  REQUIRE(g.r->shutdown().has_value());
}
