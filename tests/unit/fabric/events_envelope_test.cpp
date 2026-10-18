// FAB-INV-EVT-01 (SL-ARCH-001 ARCH1-R46): an event is a complete envelope with exactly one of
// subject id or scope tag; an incomplete one is refused with event_invalid naming the member.
// The accepted envelopes are also dumped through the bus when SL_FABRIC_EVENT_JSON_PATH is set
// (the input of `just fabric-event-schema-check`).

#include "sl/fabric/events.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

namespace {

using sl::fabric::event_about;
using sl::fabric::event_envelope;
using sl::fabric::event_spec;
using sl::fabric::id_class;
using sl::fabric::record_id;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), sl::describe(r.error()));
  return std::move(*r);
}

record_id wk(id_class c, std::uint32_t n) { return sl::fabric::well_known(c, n); }

event_spec spec_with_subject() {
  event_spec s;
  s.type = "fabric.tx.committed";
  s.schema = {.type = wk(id_class::type, 1), .major = 1, .minor = 0};
  s.actor = wk(id_class::actor, 1);
  s.about = event_about{.alt = wk(id_class::entity, 7)};
  s.correlation = sl::fabric::correlation_record{.id = 11, .scope = "tx-manager", .tick = 3};
  s.causality.tx = wk(id_class::tx, 5);
  return s;
}

event_envelope good() {
  sl::fabric::manual_clock clock(1'700'000'000'000'000);
  sl::fabric::event_source source(wk(id_class::operation, 1), clock);
  return must(source.make(spec_with_subject()));
}

void refused(const event_envelope &e, std::string_view member) {
  const auto r = sl::fabric::validate(e);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::event_invalid);
  CHECK(r.error().message.find(member) != std::string::npos);
}

} // namespace

TEST_CASE("FAB-INV-EVT-01: a complete envelope validates and round-trips through its value") {
  const event_envelope e = good();
  CHECK(sl::fabric::validate(e).has_value());
  CHECK(e.id.cls() == id_class::event);
  CHECK(e.seq == 1);
  CHECK(e.time == 1'700'000'000'000'000);
  CHECK(e.type == "fabric.tx.committed");
  CHECK(e.correlation.scope == "tx-manager");
  const auto v = must(e.to_value());
  CHECK(must(event_envelope::from_value(v)) == e);
}

TEST_CASE("FAB-INV-EVT-01: exactly one of subject id or scope tag") {
  event_envelope e = good();
  CHECK(e.about.alt.index() == 0);
  e.about.alt = std::string{"workspace"};
  CHECK(sl::fabric::validate(e).has_value());
  CHECK(e.about.alt.index() == 1);
  e.about.alt = std::string{};
  refused(e, "about");
  e.about.alt = record_id{};
  refused(e, "about");
}

TEST_CASE("FAB-INV-EVT-01: each missing or malformed member is refused naming it") {
  {
    event_envelope e = good();
    e.id = record_id{};
    refused(e, "id");
    e.id = wk(id_class::tx, 1);
    refused(e, "id");
  }
  {
    event_envelope e = good();
    e.type.clear();
    refused(e, "type");
    e.type = "has space";
    refused(e, "type");
    e.type = std::string(256, 'a');
    refused(e, "type");
  }
  {
    event_envelope e = good();
    e.schema.type = wk(id_class::entity, 1);
    refused(e, "schema.type");
  }
  {
    event_envelope e = good();
    e.seq = 0;
    refused(e, "seq");
  }
  {
    event_envelope e = good();
    e.actor = wk(id_class::entity, 1);
    refused(e, "actor");
    e.actor = record_id{};
    refused(e, "actor");
  }
  {
    event_envelope e = good();
    e.correlation.scope.clear();
    refused(e, "correlation.scope");
  }
  {
    event_envelope e = good();
    e.causality.cause = wk(id_class::tx, 1);
    refused(e, "causality.cause");
    e.causality.cause = wk(id_class::event, 1);
    e.causality.tx = wk(id_class::event, 2);
    refused(e, "causality.tx");
  }
}

TEST_CASE("FAB-INV-EVT-01: a source stamps time, seq and the current correlation") {
  sl::fabric::manual_clock clock(100);
  sl::fabric::event_source source(wk(id_class::operation, 2), clock);
  event_spec s = spec_with_subject();
  s.correlation.reset();
  s.causality.tx.reset();
  const sl::log::correlation_scope scope("job-9", 4);
  const event_envelope a = must(source.make(s));
  clock.advance_us(50);
  const event_envelope b = must(source.make(s));
  CHECK(a.seq == 1);
  CHECK(b.seq == 2);
  CHECK(b.time - a.time == 50);
  CHECK(a.correlation.scope == "job-9");
  CHECK(a.correlation.tick == 4);
  CHECK(a.correlation.id == scope.get().id);
  CHECK(source.next_seq() == 3);
}

TEST_CASE("FAB-INV-EVT-01: a refused envelope spends no seq; the payload is SLB/1") {
  sl::fabric::manual_clock clock(0);
  sl::fabric::event_source source(wk(id_class::operation, 3), clock);
  event_spec bad = spec_with_subject();
  bad.type.clear();
  const auto r = source.make(bad);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::event_invalid);
  CHECK(source.next_seq() == 1);

  event_spec s = spec_with_subject();
  s.payload = sl::fabric::value::uint(42);
  const event_envelope e = must(source.make(s));
  REQUIRE(e.payload.has_value());
  const std::vector<std::uint8_t> payload =
      *e.payload; // NOLINT(bugprone-unchecked-optional-access)
  const auto decoded = must(sl::fabric::decode(payload));
  CHECK(decoded == sl::fabric::value::uint(42));
}

TEST_CASE("FAB-INV-EVT-01: envelopes of every shape reach the JSON Lines dump") {
  sl::fabric::manual_clock clock(1'700'000'000'123'456);
  sl::fabric::event_source source(wk(id_class::operation, 4), clock);
  sl::fabric::event_bus bus;
  const auto on = bus.enable_json_from_env();
  REQUIRE(on.has_value());

  event_spec subject = spec_with_subject();
  subject.causality.cause = wk(id_class::event, 99);
  subject.causality.revision = sl::fabric::digest{};
  subject.payload = sl::fabric::value::uint(7);
  event_spec scope = spec_with_subject();
  scope.type = "fabric.workspace.opened";
  scope.about = event_about{.alt = std::string{"workspace \"main\"\n"}};
  scope.causality = {};
  scope.correlation.reset();
  event_spec bare = spec_with_subject();
  bare.type = "fabric.tx.rejected";
  bare.causality = {};
  bare.correlation = sl::fabric::correlation_record{.id = 0, .scope = "process", .tick = 0};

  CHECK(bus.publish(must(source.make(subject))).has_value());
  CHECK(bus.publish(must(source.make(scope))).has_value());
  CHECK(bus.publish(must(source.make(bare))).has_value());
  CHECK(bus.delivered() == 3);
  CHECK(bus.json_failures() == 0);
}

TEST_CASE("FAB-INV-EVT-01: the JSON line has the subject or the scope, never both") {
  const event_envelope e = good();
  const std::string line = must(sl::fabric::to_json_line(e));
  CHECK(line.find("\"subject\":\"ent_") != std::string::npos);
  CHECK(line.find("\"scope\":\"tx-manager\"") != std::string::npos); // the correlation scope
  CHECK(line.find('\n') == std::string::npos);
  event_envelope s = good();
  s.about.alt = std::string{"workspace"};
  const std::string scoped = must(sl::fabric::to_json_line(s));
  CHECK(scoped.find("\"subject\"") == std::string::npos);
  CHECK(scoped.find("\"scope\":\"workspace\"") != std::string::npos);
  event_envelope broken = good();
  broken.correlation.scope.clear();
  CHECK_FALSE(sl::fabric::to_json_line(broken).has_value());
}
