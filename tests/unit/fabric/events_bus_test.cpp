// FAB-INV-EVT-04 (SL-ARCH-001 ARCH1-R46, SL-ARCH-002 ARCH2-R13): delivery is ordered by commit
// seq; subscriptions filter by type prefix and unsubscribe by RAII; nothing is delivered for a
// refused envelope; a publish from a handler is queued behind the current event.

#include "sl/fabric/events.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/log.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"

namespace {

using sl::fabric::event_envelope;
using sl::fabric::event_spec;
using sl::fabric::id_class;

struct fixture {
  sl::fabric::manual_clock clock{1'700'000'000'000'000};
  sl::fabric::event_source source{sl::fabric::well_known(id_class::operation, 1), clock};
  sl::fabric::event_bus bus;
  std::uint64_t tick = 0;

  event_envelope make(const std::string &type) {
    event_spec s;
    s.type = type;
    s.schema = {.type = sl::fabric::well_known(id_class::type, 1), .major = 1, .minor = 0};
    s.actor = sl::fabric::well_known(id_class::actor, 1);
    s.about = sl::fabric::event_about{.alt = std::string{"workspace"}};
    s.correlation = sl::fabric::correlation_record{.id = 1, .scope = "bus-test", .tick = ++tick};
    auto r = source.make(std::move(s));
    REQUIRE_MESSAGE(r.has_value(), sl::describe(r.error()));
    clock.advance_us(10);
    return std::move(*r);
  }
};

struct recorder {
  std::vector<std::uint64_t> seqs;
  std::vector<std::string> types;
  sl::fabric::event_handler handler() {
    return [this](const event_envelope &e) noexcept {
      seqs.push_back(e.seq);
      types.push_back(e.type);
    };
  }
};

} // namespace

TEST_CASE("FAB-INV-EVT-04: events are delivered in seq order") {
  fixture f;
  recorder r;
  const auto sub = f.bus.subscribe("", r.handler());
  for (int i = 0; i < 5; ++i) {
    CHECK(f.bus.publish(f.make("fabric.tx.committed")).has_value());
  }
  CHECK(r.seqs == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
  CHECK(f.bus.last_seq() == 5);
  CHECK(f.bus.delivered() == 5);
  CHECK(sub.active());
}

TEST_CASE("FAB-INV-EVT-04: a batch is delivered sorted by seq whatever its order") {
  fixture f;
  recorder r;
  const auto sub = f.bus.subscribe("", r.handler());
  std::vector<event_envelope> batch;
  batch.reserve(6);
  for (int i = 0; i < 6; ++i) {
    batch.push_back(f.make("fabric.tx.committed"));
  }
  std::ranges::reverse(batch);
  std::swap(batch[1], batch[4]);
  CHECK(f.bus.publish_batch(std::move(batch)).has_value());
  CHECK(r.seqs == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("FAB-INV-EVT-04: a seq at or below the last accepted one is refused and not delivered") {
  fixture f;
  recorder r;
  const auto sub = f.bus.subscribe("", r.handler());
  const event_envelope a = f.make("fabric.tx.committed");
  const event_envelope b = f.make("fabric.tx.committed");
  CHECK(f.bus.publish(b).has_value());
  const auto late = f.bus.publish(a);
  REQUIRE_FALSE(late.has_value());
  CHECK(late.error().code == sl::errc::event_invalid);
  const auto again = f.bus.publish(b);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == sl::errc::event_invalid);
  CHECK(r.seqs == std::vector<std::uint64_t>{2});
  CHECK(f.bus.last_seq() == 2);
}

TEST_CASE("FAB-INV-EVT-04: a batch is all or nothing") {
  fixture f;
  recorder r;
  const auto sub = f.bus.subscribe("", r.handler());
  event_envelope a = f.make("fabric.tx.committed");
  event_envelope b = f.make("fabric.tx.committed");
  event_envelope dup = f.make("fabric.tx.committed");
  dup.seq = b.seq;
  CHECK_FALSE(f.bus.publish_batch({a, b, dup}).has_value());
  event_envelope invalid = f.make("fabric.tx.committed");
  invalid.correlation.scope.clear();
  CHECK_FALSE(f.bus.publish_batch({a, b, invalid}).has_value());
  CHECK(r.seqs.empty());
  CHECK(f.bus.last_seq() == 0);
  CHECK(f.bus.publish_batch({a, b}).has_value());
  CHECK(r.seqs == std::vector<std::uint64_t>{1, 2});
}

TEST_CASE(
    "FAB-INV-EVT-04: subscribers filter by type prefix and are called in subscription order") {
  fixture f;
  recorder all;
  recorder tx;
  recorder rejected;
  std::vector<int> order;
  const auto s1 = f.bus.subscribe("", all.handler());
  const auto s2 = f.bus.subscribe("fabric.tx.", tx.handler());
  const auto s3 = f.bus.subscribe("fabric.tx.rejected", rejected.handler());
  const auto s4 = f.bus.subscribe(
      "fabric.tx.", [&order](const event_envelope &) noexcept { order.push_back(4); });
  const auto s5 = f.bus.subscribe(
      "fabric.tx.", [&order](const event_envelope &) noexcept { order.push_back(5); });
  CHECK(f.bus.subscriber_count() == 5);
  CHECK(f.bus.publish(f.make("fabric.tx.committed")).has_value());
  CHECK(f.bus.publish(f.make("fabric.tx.rejected")).has_value());
  CHECK(f.bus.publish(f.make("fabric.history.revision")).has_value());
  CHECK(all.seqs.size() == 3);
  CHECK(tx.types == std::vector<std::string>{"fabric.tx.committed", "fabric.tx.rejected"});
  CHECK(rejected.types == std::vector<std::string>{"fabric.tx.rejected"});
  CHECK(order == std::vector<int>{4, 5, 4, 5});
}

TEST_CASE("FAB-INV-EVT-04: a subscription ends when reset, destroyed or moved over") {
  fixture f;
  recorder a;
  recorder b;
  {
    auto sub = f.bus.subscribe("", a.handler());
    CHECK(f.bus.publish(f.make("x.one")).has_value());
    sub.reset();
    CHECK_FALSE(sub.active());
    sub.reset(); // harmless
    CHECK(f.bus.publish(f.make("x.two")).has_value());
  }
  CHECK(a.seqs == std::vector<std::uint64_t>{1});
  {
    auto first = f.bus.subscribe("", b.handler());
    {
      auto second = std::move(first);
      // NOLINTNEXTLINE(bugprone-use-after-move): a moved-from handle is inactive
      CHECK_FALSE(first.active());
      CHECK(second.active());
      CHECK(f.bus.subscriber_count() == 1);
    }
    CHECK(f.bus.subscriber_count() == 0);
  }
  CHECK(f.bus.publish(f.make("x.three")).has_value());
  CHECK(b.seqs.empty());
}

TEST_CASE("FAB-INV-EVT-04: a subscription may outlive the bus; a handler may unsubscribe itself") {
  sl::fabric::subscription outlive;
  {
    fixture f;
    outlive = f.bus.subscribe("", [](const event_envelope &) noexcept {});
    CHECK(outlive.active());
  }
  CHECK_FALSE(outlive.active());
  outlive.reset();

  fixture f;
  sl::fabric::subscription self;
  int calls = 0;
  self = f.bus.subscribe("", [&](const event_envelope &) noexcept {
    ++calls;
    self.reset();
  });
  CHECK(f.bus.publish(f.make("x.one")).has_value());
  CHECK(f.bus.publish(f.make("x.two")).has_value());
  CHECK(calls == 1);
}

TEST_CASE("FAB-INV-EVT-04: a publish from a handler is delivered after the current event") {
  fixture f;
  std::vector<std::string> log;
  const event_envelope follow_up = f.make("x.follow-up");
  const event_envelope first = f.make("x.first");
  const event_envelope third = f.make("x.third");
  const auto sub = f.bus.subscribe("", [&](const event_envelope &e) noexcept {
    log.push_back("begin " + e.type);
    if (e.type == "x.first") {
      const auto r = f.bus.publish(third);
      log.emplace_back(r.has_value() ? "queued" : "refused");
    }
    log.push_back("end " + e.type);
  });
  // seqs: follow_up 1, first 2, third 3. Publish first (2) then the handler publishes third (3).
  CHECK(f.bus.publish(first).has_value());
  CHECK(log == std::vector<std::string>{"begin x.first", "queued", "end x.first", "begin x.third",
                                        "end x.third"});
  CHECK(f.bus.publish(follow_up).error().code == sl::errc::event_invalid); // seq 1 is behind
}

TEST_CASE("FAB-INV-EVT-04: the bus dumps JSON Lines and forwards to a logger in delivery order") {
  fixture f;
  sl::log::logger target;
  auto sink = std::make_shared<sl::log::memory_sink>(16);
  target.add_sink(sink);
  f.bus.forward_to_log(&target);
  const auto on = f.bus.enable_json_from_env();
  REQUIRE(on.has_value());
  std::vector<event_envelope> batch;
  batch.reserve(4);
  for (int i = 0; i < 4; ++i) {
    batch.push_back(f.make("fabric.tx.committed"));
  }
  std::ranges::reverse(batch);
  CHECK(f.bus.publish_batch(std::move(batch)).has_value());
  const auto records = sink->records();
  REQUIRE(records.size() == 4);
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto *seq = records[i].find("seq");
    REQUIRE(seq != nullptr);
    CHECK(std::get<std::int64_t>(*seq) == static_cast<std::int64_t>(i + 1));
    CHECK(records[i].scope == "bus-test");
  }
  CHECK(f.bus.json_failures() == 0);
  f.bus.forward_to_log(nullptr);
  f.bus.disable_json_lines();
  CHECK(f.bus.publish(f.make("fabric.tx.committed")).has_value());
  CHECK(sink->size() == 4);
}

TEST_CASE("FAB-INV-EVT-04: an unwritable dump path is io_error") {
  fixture f;
  const auto bad = f.bus.enable_json_lines("/nonexistent-dir-for-sl-events/x.jsonl");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::io_error);
}
