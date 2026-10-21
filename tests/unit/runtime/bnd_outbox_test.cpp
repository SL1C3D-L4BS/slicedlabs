// RT-INV-BND-04 (SL-ARCH-003 ARCH3-R154, ADR-0027): the outbox merges its per-job buffers by
// ordered_key and keeps, for each (record, path), the value of the largest key, ordered by
// (record id bytes, path): over 500 seeded distributions of changes across buffers the drain
// equals an oracle built from the keys alone. Transient data never reaches a proposal: a
// component without a binding cannot be observed (link_transient_component), and writes that only
// touch a bound component's runtime-only bytes or a transient component propose nothing.

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/replay.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace fab = sl::fabric;
using rtt::fabric_host;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

struct splitmix {
  std::uint64_t state;
  std::uint64_t next() noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint32_t below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }
};

struct keyed {
  det::ordered_key key;
  std::uint32_t buffer = 0;
  fab::observed_change change;
};

using target = std::pair<fab::record_id::bytes_type, fab::path>;

// One seeded distribution: drain() against the oracle.
void check_seed(std::uint64_t seed) {
  splitmix rng{seed};
  const std::uint32_t buffers = 1 + rng.below(4);
  const std::uint32_t count = rng.below(48);
  std::vector<keyed> all;
  for (std::uint32_t k = 0; k < count; ++k) {
    // Unique keys: local_seq is the draw's position; the rest varies the merge order.
    const det::ordered_key key{.tick = 1 + rng.below(2),
                               .phase = static_cast<std::uint8_t>(rng.below(3)),
                               .system_index = static_cast<std::uint16_t>(rng.below(3)),
                               .chunk_index = rng.below(3),
                               .local_seq = k};
    fab::observed_change c{.record = fabric_host::body_id(rng.below(5)),
                           .path = {fab::path_segment::field(1 + rng.below(3))},
                           .value = fab::value::uint(rng.next())};
    all.push_back(keyed{.key = key, .buffer = rng.below(buffers), .change = std::move(c)});
  }
  // Each buffer gets its changes in increasing key order.
  std::ranges::sort(all, [](const keyed &a, const keyed &b) { return a.key < b.key; });
  link::outbox out;
  for (std::uint32_t b = 0; b < buffers; ++b) {
    static_cast<void>(out.add_buffer());
  }
  std::map<target, const keyed *> oracle;
  for (const keyed &k : all) {
    out.buffer_at(k.buffer).push(k.key, k.change);
    const target t{k.change.record.bytes(), k.change.path};
    const auto at = oracle.find(t);
    if (at == oracle.end() || at->second->key < k.key) {
      oracle.insert_or_assign(t, &k);
    }
  }
  CHECK(out.pending() == all.size());
  std::vector<fab::observed_change> want;
  want.reserve(oracle.size());
  for (const auto &[t, k] : oracle) {
    want.push_back(k->change);
  }
  const std::vector<fab::observed_change> got = out.drain();
  CHECK_MESSAGE(got == want, "seed " << seed);
  CHECK(out.pending() == 0);
  CHECK(out.drain().empty());
}

} // namespace

TEST_CASE("RT-INV-BND-04: the drain keeps the last value per (record, path), seeds 0 to 249") {
  for (std::uint64_t seed = 0; seed < 250; ++seed) {
    check_seed(seed);
  }
}

TEST_CASE("RT-INV-BND-04: the drain keeps the last value per (record, path), seeds 250 to 499") {
  for (std::uint64_t seed = 250; seed < 500; ++seed) {
    check_seed(seed);
  }
}

TEST_CASE("RT-INV-BND-04: transient components and runtime-only bytes never reach a proposal") {
  rtt::rig_options o;
  o.host.levels = 1;
  o.host.bodies = 3;
  o.preview_sink = true;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  const sl::result<void> spin = rig->link->observe_component(rig->spin_component);
  REQUIRE_FALSE(spin.has_value());
  CHECK(spin.error().code == sl::errc::link_transient_component);
  must_ok(rig->link->observe_component(rig->body_component));
  must_ok(rig->link->observe_component(rig->body_component)); // twice is ok

  ecs::world &w = *rig->stack->world;
  const ecs::entity e = must(rig->entity_of(fabric_host::body_id(1)));
  must_ok(w.add(e, rtt::bnd_spin{.rate = 1.0F}));
  must_ok(rig->build());
  static_cast<void>(must(rig->step()));
  CHECK(rig->preview.submissions() == 0);

  // Only the spare bytes and the spin: the row is observed, nothing differs from the projection.
  rtt::bnd_body body = *must(w.get<rtt::bnd_body>(e));
  body.spare = 9;
  must_ok(w.set(e, body));
  must_ok(w.set(e, rtt::bnd_spin{.rate = 2.0F}));
  static_cast<void>(must(rig->step()));
  CHECK(rig->preview.submissions() == 0);
  CHECK(rig->link->stats().echo_suppressed > 0);

  // A bound field: exactly that path is proposed.
  body.mass = 123.0F;
  body.spare = 10;
  must_ok(w.set(e, body));
  must_ok(w.set(e, rtt::bnd_spin{.rate = 3.0F}));
  static_cast<void>(must(rig->step()));
  REQUIRE(rig->preview.submissions() == 1);
  const std::optional<fab::patch> &last = rig->preview.last();
  REQUIRE(last.has_value());
  const fab::patch &p = *last; // NOLINT(bugprone-unchecked-optional-access): REQUIRE above
  REQUIRE(p.ops.size() == 1);
  CHECK(p.ops[0].target == fabric_host::body_id(1));
  CHECK(p.ops[0].path == fab::path{fab::path_segment::field(1)});
  CHECK(p.ops[0].new_value == std::optional<fab::value>(must(fab::value::f32(123.0F))));
  CHECK(rig->link->stats().proposed_patches == 1);
  CHECK(rig->link->stats().proposed_changes == 1);
}
