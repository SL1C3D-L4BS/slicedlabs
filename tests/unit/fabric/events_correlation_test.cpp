// FAB-INV-EVT-03 (SL-ARCH-001 ARCH1-R48): the correlation maps field for field to
// sl::log::correlation, and the record owns its scope string.

#include "sl/fabric/events.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "sl/core/log.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"

namespace {

using sl::fabric::correlation_record;

sl::fabric::event_envelope sample(correlation_record c) {
  sl::fabric::manual_clock clock(1'700'000'000'000'000);
  sl::fabric::event_source source(sl::fabric::well_known(sl::fabric::id_class::operation, 1),
                                  clock);
  sl::fabric::event_spec s;
  s.type = "fabric.tx.committed";
  s.schema = {
      .type = sl::fabric::well_known(sl::fabric::id_class::type, 1), .major = 1, .minor = 0};
  s.actor = sl::fabric::well_known(sl::fabric::id_class::actor, 1);
  s.about = sl::fabric::event_about{.alt = std::string{"workspace"}};
  s.correlation = std::move(c);
  auto r = source.make(std::move(s));
  REQUIRE_MESSAGE(r.has_value(), sl::describe(r.error()));
  return std::move(*r);
}

} // namespace

TEST_CASE("FAB-INV-EVT-03: to_log and from_log map id, scope and tick field for field") {
  const correlation_record c{.id = 0xFEDCBA9876543210ULL, .scope = "entity:42", .tick = 123456};
  const sl::log::correlation l = sl::fabric::to_log(c);
  CHECK(l.id == c.id);
  CHECK(l.scope == "entity:42");
  CHECK(l.tick == c.tick);
  CHECK(l.scope.data() == c.scope.data()); // a view of the owned string
  CHECK(sl::fabric::from_log(l) == c);

  const sl::log::correlation process{};
  const correlation_record from_default = sl::fabric::from_log(process);
  CHECK(from_default.id == 0);
  CHECK(from_default.scope == "process");
  CHECK(from_default.tick == 0);
  const sl::log::correlation back = sl::fabric::to_log(from_default);
  CHECK(back.id == process.id);
  CHECK(back.scope == process.scope);
  CHECK(back.tick == process.tick);
}

TEST_CASE("FAB-INV-EVT-03: the record owns its scope; the log view's backing may die") {
  correlation_record owned;
  {
    auto backing = std::make_unique<std::string>("short-lived-scope-tag-that-is-not-sso");
    owned = sl::fabric::from_log(
        sl::log::correlation{.id = 5, .scope = std::string_view{*backing}, .tick = 6});
    backing->assign(backing->size(), 'x'); // overwrite, then free
  }
  CHECK(owned.scope == "short-lived-scope-tag-that-is-not-sso");
  CHECK(owned.id == 5);
  CHECK(owned.tick == 6);
}

TEST_CASE("FAB-INV-EVT-03: the current correlation is captured from the innermost scope") {
  {
    const correlation_record outside = sl::fabric::current_correlation_record();
    CHECK(outside.id == 0);
    CHECK(outside.scope == sl::log::process_scope());
  }
  const sl::log::correlation_scope outer("outer", 1);
  const sl::log::correlation_scope inner("inner", 2);
  const correlation_record c = sl::fabric::current_correlation_record();
  CHECK(c.id == inner.get().id);
  CHECK(c.scope == "inner");
  CHECK(c.tick == 2);
  CHECK(sl::fabric::to_log(c).id == sl::log::current_correlation().id);
}

TEST_CASE("FAB-INV-EVT-03: log_event emits a log event carrying the envelope's correlation") {
  const sl::fabric::event_envelope e =
      sample(correlation_record{.id = 31, .scope = "job:render", .tick = 77});
  sl::log::logger target;
  auto sink = std::make_shared<sl::log::memory_sink>(8);
  target.add_sink(sink);
  target.set_min_level(sl::log::level::trace);
  sl::fabric::log_event(target, e);
  const auto records = sink->records();
  REQUIRE(records.size() == 1);
  const auto &r = records.front();
  CHECK(r.system == "fabric.event");
  CHECK(r.message == "fabric.tx.committed");
  CHECK(r.correlation_id == 31);
  CHECK(r.scope == "job:render");
  CHECK(r.tick == 77);
  const auto *seq = r.find("seq");
  REQUIRE(seq != nullptr);
  CHECK(std::get<std::int64_t>(*seq) == 1);
  const auto *scope = r.find("scope");
  REQUIRE(scope != nullptr);
  CHECK(std::get<std::string>(*scope) == "workspace");
  CHECK(r.find("event_id") != nullptr);
  CHECK(std::chrono::duration_cast<std::chrono::microseconds>(r.ts.time_since_epoch()).count() ==
        1'700'000'000'000'000);

  const std::string json = sl::log::json_sink::format(sl::log::event{
      .level = sl::log::level::info,
      .system = r.system,
      .message = r.message,
      .fields = {},
      .correlation = sl::fabric::to_log(e.correlation),
      .where = std::source_location::current(),
      .ts = {},
  });
  CHECK(json.find("\"correlation\":{\"id\":31,\"scope\":\"job:render\",\"tick\":77}") !=
        std::string::npos);
}
