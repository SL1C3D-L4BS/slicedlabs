// FAB-INV-EVT-02 (SL-ARCH-001 ARCH1-R47): event ids derive from (tx id, kind, seq), so replay
// reproduces them, and neither time nor correlation nor actor nor payload enters the id.

#include "sl/fabric/events.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

namespace {

using sl::fabric::event_about;
using sl::fabric::event_envelope;
using sl::fabric::event_spec;
using sl::fabric::id_class;
using sl::fabric::record_id;

record_id wk(id_class c, std::uint32_t n) { return sl::fabric::well_known(c, n); }

event_spec spec(const record_id &tx) {
  event_spec s;
  s.type = "fabric.tx.committed";
  s.schema = {.type = wk(id_class::type, 1), .major = 1, .minor = 0};
  s.actor = wk(id_class::actor, 1);
  s.about = event_about{.alt = wk(id_class::entity, 7)};
  s.causality.tx = tx;
  return s;
}

event_envelope make(sl::fabric::event_source &source, event_spec s) {
  auto r = source.make(std::move(s));
  REQUIRE_MESSAGE(r.has_value(), sl::describe(r.error()));
  return std::move(*r);
}

} // namespace

TEST_CASE(
    "FAB-INV-EVT-02: an event id is derive(event, tx, kind:seq), equal to the hashlib value") {
  const record_id tx = wk(id_class::tx, 5);
  CHECK(tx.to_string() == "txn_080C0000000000000000000005");
  const record_id id = sl::fabric::event_id_for(tx, "fabric.tx.committed", 7);
  CHECK(id == sl::fabric::derive(id_class::event, tx, "fabric.tx.committed:7"));
  CHECK(id.cls() == id_class::event);
  CHECK(id.scheme() == sl::fabric::mint_scheme::derived);
  CHECK(id.to_string() == "evt_090A02JWSJBNWYYT5ZCHMV2MKZ");
  const record_id source = wk(id_class::operation, 1);
  CHECK(sl::fabric::event_id_for(source, "fabric.scope.opened", 1).to_string() ==
        "evt_090885AG57MZ0RE3GQ2K7J8V73");
}

TEST_CASE("FAB-INV-EVT-02: every input of the derivation changes the id") {
  const record_id tx = wk(id_class::tx, 5);
  const record_id base = sl::fabric::event_id_for(tx, "fabric.tx.committed", 7);
  CHECK(base != sl::fabric::event_id_for(wk(id_class::tx, 6), "fabric.tx.committed", 7));
  CHECK(base != sl::fabric::event_id_for(tx, "fabric.tx.rejected", 7));
  CHECK(base != sl::fabric::event_id_for(tx, "fabric.tx.committed", 8));
  CHECK(base == sl::fabric::event_id_for(tx, "fabric.tx.committed", 7));
  std::set<record_id> seen;
  for (std::uint64_t seq = 1; seq <= 1000; ++seq) {
    seen.insert(sl::fabric::event_id_for(tx, "fabric.tx.committed", seq));
  }
  CHECK(seen.size() == 1000);
}

TEST_CASE("FAB-INV-EVT-02: replay reproduces the ids whatever the time, correlation and payload") {
  const record_id tx = wk(id_class::tx, 5);
  sl::fabric::manual_clock first_clock(1'000);
  sl::fabric::manual_clock replay_clock(9'999'999);
  sl::fabric::event_source first(wk(id_class::operation, 1), first_clock);
  sl::fabric::event_source replay(wk(id_class::operation, 2), replay_clock); // another source id
  for (int i = 0; i < 5; ++i) {
    event_spec a = spec(tx);
    a.correlation = sl::fabric::correlation_record{.id = 1, .scope = "live", .tick = 10};
    a.payload = sl::fabric::value::uint(1);
    event_spec b = spec(tx);
    b.correlation = sl::fabric::correlation_record{.id = 77, .scope = "replay", .tick = 99};
    b.actor = wk(id_class::actor, 2);
    b.payload = sl::fabric::value::uint(2);
    first_clock.advance_us(13);
    const event_envelope ea = make(first, std::move(a));
    const event_envelope eb = make(replay, std::move(b));
    CHECK(ea.id == eb.id);
    CHECK(ea.time != eb.time);
    CHECK(ea.correlation != eb.correlation);
  }
}

TEST_CASE("FAB-INV-EVT-02: without a transaction the source id is the parent") {
  sl::fabric::manual_clock clock(0);
  sl::fabric::event_source one(wk(id_class::operation, 1), clock);
  sl::fabric::event_source two(wk(id_class::operation, 2), clock);
  event_spec s = spec(record_id{});
  s.causality.tx.reset();
  const event_envelope a = make(one, s);
  const event_envelope b = make(two, s);
  CHECK(a.id == sl::fabric::event_id_for(wk(id_class::operation, 1), "fabric.tx.committed", 1));
  CHECK(a.id != b.id);
  event_spec with_tx = spec(wk(id_class::tx, 5));
  const event_envelope c = make(one, with_tx);
  CHECK(c.id == sl::fabric::event_id_for(wk(id_class::tx, 5), "fabric.tx.committed", 2));
}
