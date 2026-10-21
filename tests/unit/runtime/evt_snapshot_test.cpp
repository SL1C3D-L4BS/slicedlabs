// RT-INV-EVT-07 (SL-ARCH-003 ARCH3-R091, `determinism` pending events: snapshotted (contributor),
// hashed without correlation): runtime.events version 1 captures both frame lists of every
// channel; a restore into fresh channels reproduces the envelopes, causes and payloads, the same
// bytes and the same hash. Events written under other correlation ids give identical bytes and
// hash, a transient payload field changes neither, and truncated, trailing or miscounted bytes
// are snapshot_corrupt with the state unchanged.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace det = sl::runtime::det;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct ping {
  std::uint32_t from = 0;
  std::int32_t value = 0;
  std::uint32_t scratch = 0; // transient: neither captured nor hashed
};

struct pong {
  double at = 0.0;
};

rt::type_info ping_info() {
  rt::type_builder<ping> b("test.evt.ping");
  SL_RT_FIELD(b, ping, from, kept);
  SL_RT_FIELD(b, ping, value, kept);
  SL_RT_FIELD(b, ping, scratch, rt::field_flags::transient);
  return b.build();
}

rt::type_info pong_info() {
  rt::type_builder<pong> b("test.evt.pong");
  SL_RT_FIELD(b, pong, at, kept);
  return b.build();
}

struct world_of_events {
  rt::event_registry reg;
  std::unique_ptr<rt::event_channel<ping>> pings;
  std::unique_ptr<rt::event_channel<pong>> pongs;
  std::unique_ptr<rt::events_contributor> contributor;

  world_of_events() {
    REQUIRE(reg.add<ping>("ping", ping_info()).has_value());
    REQUIRE(reg.add<pong>("pong", pong_info()).has_value());
    REQUIRE(reg.freeze().has_value());
    auto a = rt::event_channel<ping>::make(reg);
    auto b = rt::event_channel<pong>::make(reg);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    pings = std::make_unique<rt::event_channel<ping>>(std::move(*a));
    pongs = std::make_unique<rt::event_channel<pong>>(std::move(*b));
    const std::array<rt::event_stream *, 2> streams{&pongs->stream(), &pings->stream()};
    contributor = std::make_unique<rt::events_contributor>(streams);
  }

  // Frame 7 then frame 8 (open), under correlation ids base, base + 1; frame 8 events cite
  // frame 7 events as causes.
  void fill(std::uint64_t correlation_base, std::uint32_t scratch) const {
    const rt::writer_key w{.tick = 70, .ph = rt::phase::tick, .system_index = 4, .chunk_index = 1};
    {
      const sl::log::correlation_scope scope(
          sl::log::correlation{.id = correlation_base, .scope = "runtime/a", .tick = 70});
      REQUIRE(pings->write(w, ping{.from = 1, .value = -5, .scratch = scratch}).has_value());
      REQUIRE(pings->write(w, ping{.from = 2, .value = 6, .scratch = scratch}).has_value());
      REQUIRE(pongs->write(w, pong{.at = 0.25}).has_value());
    }
    pings->stream().deliver(7);
    pongs->stream().deliver(7);
    pings->stream().end_frame();
    pongs->stream().end_frame();
    const rt::writer_key w2{
        .tick = 80, .ph = rt::phase::post_tick, .system_index = 9, .chunk_index = 0};
    {
      const sl::log::correlation_scope scope(
          sl::log::correlation{.id = correlation_base + 1, .scope = "runtime/b", .tick = 80});
      const rt::event_ref cause{.frame = 7, .type = pings->type(), .seq = 2};
      REQUIRE(pings->write(w2, ping{.from = 3, .value = 7, .scratch = scratch}, cause).has_value());
      REQUIRE(pongs->write(w2, pong{.at = -0.0}, cause).has_value());
    }
    pings->stream().deliver(8);
    pongs->stream().deliver(8);
  }

  [[nodiscard]] std::vector<std::byte> bytes() const {
    std::vector<std::byte> out;
    REQUIRE(contributor->capture(out).has_value());
    return out;
  }

  [[nodiscard]] sl::fabric::digest digest() const {
    det::state_hasher h;
    contributor->hash(h);
    return h.finish();
  }
};

struct seen_ping {
  rt::event_ref ref;
  std::uint32_t system_index = 0;
  std::uint64_t tick = 0;
  std::optional<rt::event_ref> cause;
  std::uint64_t correlation_id = 0;
  ping payload;
};

std::vector<seen_ping> pings_of(const world_of_events &w) {
  std::vector<seen_ping> out;
  w.pings->read_refs([&out](const rt::event_ref &ref, const rt::event_envelope &e, const ping &p) {
    out.push_back(seen_ping{.ref = ref,
                            .system_index = e.system_index,
                            .tick = e.tick,
                            .cause = e.cause,
                            .correlation_id = e.correlation.id,
                            .payload = p});
  });
  return out;
}

} // namespace

TEST_CASE("RT-INV-EVT-07: the pending-event contributor round trips") {
  const world_of_events a;
  a.fill(100, 0);
  CHECK(a.contributor->name() == "runtime.events");
  CHECK(a.contributor->version() == 1);
  const std::vector<std::byte> bytes = a.bytes();

  const world_of_events b;
  REQUIRE(b.contributor->restore(bytes).has_value());
  CHECK(b.bytes() == bytes);
  CHECK(b.digest() == a.digest());

  const std::vector<seen_ping> before = pings_of(a);
  const std::vector<seen_ping> after = pings_of(b);
  REQUIRE(before.size() == 3);
  REQUIRE(after.size() == before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    CHECK(after[i].ref == before[i].ref);
    CHECK(after[i].system_index == before[i].system_index);
    CHECK(after[i].tick == before[i].tick);
    CHECK(after[i].cause == before[i].cause);
    CHECK(after[i].payload.from == before[i].payload.from);
    CHECK(after[i].payload.value == before[i].payload.value);
    CHECK(after[i].correlation_id == 0); // correlation is not captured
  }
  CHECK(before[2].ref == rt::event_ref{.frame = 8, .type = a.pings->type(), .seq = 1});
  REQUIRE(before[2].cause.has_value());
  CHECK(before[2].cause.value_or(rt::event_ref{}).seq == 2);

  // The restored lists keep their window: frame 7 drops at the end of frame 8.
  b.pings->stream().end_frame();
  CHECK(pings_of(b).size() == 1);
}

TEST_CASE("RT-INV-EVT-07: bytes and hash exclude correlation and transient fields") {
  const world_of_events a;
  a.fill(100, 0);
  const world_of_events b;
  b.fill(0xDEAD'BEEF'0000ULL, 0x5555);
  CHECK(pings_of(a)[0].correlation_id == 100);
  CHECK(pings_of(b)[0].correlation_id == 0xDEAD'BEEF'0000ULL);
  CHECK(a.bytes() == b.bytes());
  CHECK(a.digest() == b.digest());

  // A hashed field does change both.
  const world_of_events c;
  c.fill(100, 0);
  const rt::writer_key w{.tick = 80, .ph = rt::phase::observe, .system_index = 0, .chunk_index = 0};
  REQUIRE(c.pings->write(w, ping{.from = 9, .value = 1, .scratch = 0}).has_value());
  c.pings->stream().deliver(8);
  CHECK(c.bytes() != a.bytes());
  CHECK(c.digest() != a.digest());
}

TEST_CASE("RT-INV-EVT-07: corrupt bytes are snapshot_corrupt and change nothing") {
  const world_of_events a;
  a.fill(1, 0);
  const std::vector<std::byte> good = a.bytes();
  const world_of_events b;
  b.fill(1, 0);
  REQUIRE(b.pings
              ->write({.tick = 81, .ph = rt::phase::observe, .system_index = 0, .chunk_index = 0},
                      ping{.from = 42, .value = 0, .scratch = 0})
              .has_value());
  b.pings->stream().deliver(8);
  const std::vector<std::byte> keep = b.bytes();

  std::vector<std::vector<std::byte>> bad;
  bad.emplace_back(good.begin(), good.end() - 3); // truncated
  bad.push_back(good);
  bad.back().push_back(std::byte{0}); // trailing
  bad.push_back(good);
  bad.back()[0] = std::byte{3}; // channel count
  bad.push_back(good);
  bad.back()[4] = std::byte{9}; // first channel's type id
  bad.emplace_back();
  for (const std::vector<std::byte> &bytes : bad) {
    const auto r = b.contributor->restore(bytes);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::snapshot_corrupt);
    // cppcheck-suppress knownConditionTrueFalse ; the restore above must leave the state unchanged
    CHECK(b.bytes() == keep);
  }
}
