// RT-INV-CLOCK-05 (SL-ARCH-003 ARCH3-R083, R068, master plan section 3 F6): inside a tick_scope the
// calling thread's sl::log correlation tick equals the tick and its scope is runtime/<instance>;
// the id is the enclosing correlation's when it has one and a fresh non-zero id otherwise; scopes
// nest and restore, they work on any thread and do not leak to other threads, and a logged event
// carries the tick and the scope.

#include "sl/core/log.hpp"
#include "sl/runtime/time.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace log = sl::log;
namespace time = sl::runtime::time;

} // namespace

TEST_CASE("RT-INV-CLOCK-05: inside a tick_scope the correlation tick and scope are the runtime's") {
  const log::correlation outside = log::current_correlation();
  CHECK(outside.id == 0);
  {
    const time::tick_scope ts(42, "runtime/main");
    const log::correlation c = log::current_correlation();
    CHECK(c.tick == 42);
    CHECK(c.scope == "runtime/main");
    CHECK(c.id != 0);
    CHECK(ts.get().id == c.id);
    {
      const time::tick_scope inner(43, "runtime/main");
      CHECK(log::current_correlation().tick == 43);
      CHECK(log::current_correlation().id == c.id); // the enclosing id is kept
    }
    CHECK(log::current_correlation().tick == 42);
  }
  CHECK(log::current_correlation().id == 0);
  CHECK(log::current_correlation().scope == outside.scope);
  CHECK(log::current_correlation().tick == 0);
}

TEST_CASE("RT-INV-CLOCK-05: a tick_scope keeps a frame's correlation id") {
  const log::correlation_scope frame(log::correlation{.id = 7777, .scope = "frame", .tick = 0});
  const time::tick_scope ts(9, "runtime/editor");
  const log::correlation c = log::current_correlation();
  CHECK(c.id == 7777);
  CHECK(c.tick == 9);
  CHECK(c.scope == "runtime/editor");
}

TEST_CASE("RT-INV-CLOCK-05: tick scopes are per thread and logged events carry the tick") {
  std::uint64_t worker_tick = 1;
  std::string worker_scope;
  std::uint64_t worker_outside_tick = 1;
  {
    const time::tick_scope ts(100, "runtime/a");
    std::thread worker([&] noexcept {
      worker_outside_tick = log::current_correlation().tick; // not inherited by a bare thread
      const time::tick_scope mine(200, "runtime/b");
      worker_tick = log::current_correlation().tick;
      worker_scope = std::string(log::current_correlation().scope);
    });
    worker.join();
    CHECK(log::current_correlation().tick == 100);
    CHECK(log::current_correlation().scope == "runtime/a");

    log::logger logger;
    auto sink = std::make_shared<log::memory_sink>(4);
    logger.add_sink(sink);
    log::emit(logger, log::level::info, "runtime", "tick");
    const auto events = sink->records();
    REQUIRE(events.size() == 1);
    CHECK(events[0].tick == 100);
    CHECK(events[0].scope == "runtime/a");
  }
  CHECK(worker_outside_tick == 0);
  CHECK(worker_tick == 200);
  CHECK(worker_scope == "runtime/b");
}
