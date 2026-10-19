// FAB-INV-F5-02 (SL-ARCH-002 ARCH2-R17, ADR-0016): a rogue commit through testing::backdoor is a
// bypass: the ledger counts it in `mutations` and `bypassing`, a fatal "fabric/f5" event names
// the site, and on a store a live transaction manager governs SL_DEBUG_ASSERT trips (captured
// here by a scoped handler). On a store no manager governs (how tests seed bare stores) it is
// counted and logged at debug level, below the default, without the assertion (WO-064), so a
// passing test that seeds through the backdoor prints no "fabric/f5" line. The log backend's commit
// and intent writes are both mutating sites. Builds without SL_FABRIC_F5_INSTRUMENT (Release)
// compile the ledger out: the test then asserts !f5::enabled and that nothing is counted.

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/core/version.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

struct captured_asserts {
  std::uint64_t count = 0;
  std::string last;
  sl::scoped_assert_handler handler{[this](const sl::assert_report &r) {
    ++count;
    last = std::string(r.message);
  }};
};

// A memory sink on the default logger for the test's lifetime.
struct captured_log {
  std::shared_ptr<sl::log::memory_sink> sink = std::make_shared<sl::log::memory_sink>(64);
  captured_log() { sl::log::default_logger().add_sink(sink); }
  captured_log(const captured_log &) = delete;
  captured_log(captured_log &&) = delete;
  captured_log &operator=(const captured_log &) = delete;
  captured_log &operator=(captured_log &&) = delete;
  ~captured_log() { sl::log::default_logger().remove_sink(sink.get()); }

  [[nodiscard]] std::vector<sl::log::memory_sink::record> f5_events() const {
    std::vector<sl::log::memory_sink::record> out;
    for (auto &r : sink->records()) {
      if (r.system == f5::log_system) {
        out.push_back(std::move(r));
      }
    }
    return out;
  }
};

std::string string_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v)
                                                                 : std::string();
}

bool bool_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<bool>(*v) && std::get<bool>(*v);
}

commit_batch blob_batch(char tag) {
  commit_batch b;
  const std::vector<std::uint8_t> bytes = {static_cast<std::uint8_t>(tag)};
  b.objects.push_back(make_object(object_kind::blob, bytes));
  return b;
}

} // namespace

TEST_CASE("FAB-INV-F5-02: the ledger is compiled in exactly for Debug and sanitizer builds") {
  const sl::core::BuildInfo bi = sl::core::build_info();
  const bool instrumented = bi.build_type == "Debug" || bi.sanitizer != "none";
  INFO("build type " << std::string(bi.build_type) << ", sanitizer " << std::string(bi.sanitizer));
  CHECK(f5::enabled == instrumented);
  if constexpr (!f5::enabled) {
    // Release: no counters, no log, no assertion; the passkey alone carries F5.
    captured_asserts asserts;
    captured_log log;
    memory_backend store;
    f5::reset_for_test();
    REQUIRE(tg::backdoor::commit(store, blob_batch('r')).has_value());
    CHECK(f5::read() == f5::ledger{});
    CHECK(f5::last_bypass_site().empty());
    CHECK(log.f5_events().empty());
    CHECK(asserts.count == 0);
  }
}

TEST_CASE("FAB-INV-F5-02: a backdoor commit on a governed store is counted, logged and asserted") {
  if constexpr (!f5::enabled) {
    CHECK_FALSE(f5::enabled);
    return;
  }
  captured_asserts asserts;
  captured_log log;
  auto store = std::make_shared<memory_backend>();
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  f5::reset_for_test();

  REQUIRE(tg::backdoor::commit(*store, blob_batch('g')).has_value());
  CHECK(f5::read() == f5::ledger{.mutations = 1, .bypassing = 1});
  CHECK(f5::last_bypass_site() == "memory_backend::commit");
  const auto events = log.f5_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].level == sl::log::level::fatal);
  CHECK(string_field(events[0], "site") == "memory_backend::commit");
  CHECK(bool_field(events[0], "governed"));
#ifndef NDEBUG
  CHECK(asserts.count == 1);
  CHECK(asserts.last.contains("memory_backend::commit"));
  CHECK(asserts.last.contains("bypassed the transaction manager"));
#endif

  // A commit through the manager afterwards is a mutation, not a bypass.
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  static_cast<void>(must(tg::commit_ops(ws->manager(), editor, {must(tg::create_entity("a"))})));
  CHECK(f5::read().mutations >= 2);
  CHECK(f5::read().bypassing == 1);
  CHECK(log.f5_events().size() == 1);
}

TEST_CASE("FAB-INV-F5-02: a bypass on an ungoverned store is counted and logged at debug, not "
          "asserted") {
  if constexpr (!f5::enabled) {
    CHECK_FALSE(f5::enabled);
    return;
  }
  captured_asserts asserts;
  captured_log log;
  auto store = std::make_shared<memory_backend>();
  {
    // Governed while a workspace is open over it, ungoverned once it is gone.
    auto clock = std::make_shared<manual_clock>(1'000'000);
    auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  }
  f5::reset_for_test();
  // At the default level the seed prints no "fabric/f5" line at all.
  REQUIRE(tg::backdoor::commit(*store, blob_batch('u')).has_value());
  CHECK(f5::read() == f5::ledger{.mutations = 1, .bypassing = 1});
  CHECK(log.f5_events().empty());
  CHECK(asserts.count == 0);

  // It is still logged, at debug: visible when the logger is lowered.
  sl::log::logger &logger = sl::log::default_logger();
  const sl::log::level before = logger.min_level();
  logger.set_min_level(sl::log::level::debug);
  REQUIRE(tg::backdoor::commit(*store, blob_batch('v')).has_value());
  logger.set_min_level(before);
  const auto events = log.f5_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].level == sl::log::level::debug);
  CHECK_FALSE(bool_field(events[0], "governed"));
  CHECK(asserts.count == 0);
  CHECK(f5::read() == f5::ledger{.mutations = 2, .bypassing = 2});

  // A refused rogue commit still counts: the call itself is the bypass.
  CHECK_FALSE(tg::backdoor::commit(*store, commit_batch{}).has_value());
  CHECK(f5::read() == f5::ledger{.mutations = 3, .bypassing = 3});
}

TEST_CASE("FAB-INV-F5-02: log_backend commit and intent writes are counted mutating sites") {
  if constexpr (!f5::enabled) {
    CHECK_FALSE(f5::enabled);
    return;
  }
  captured_asserts asserts;
  captured_log log;
  tg::tx_log_store log_store;
  auto store = must(log_store.open());
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  f5::reset_for_test();

  commit_batch b = blob_batch('i');
  b.intents.push_back(intent_record{.operation = tg::test_id(id_class::operation, "f5-intent"),
                                    .kind = "f5.rogue",
                                    .phase = intent_phase::begin,
                                    .outcome = intent_outcome::none});
  REQUIRE(tg::backdoor::commit(*store, std::move(b)).has_value());
  CHECK(f5::read() == f5::ledger{.mutations = 2, .bypassing = 2});
  CHECK(f5::last_bypass_site() == "log_backend::commit intents");
  const auto events = log.f5_events();
  REQUIRE(events.size() == 2);
  CHECK(string_field(events[0], "site") == "log_backend::commit");
  CHECK(string_field(events[1], "site") == "log_backend::commit intents");
#ifndef NDEBUG
  CHECK(asserts.count == 2);
#endif
}
