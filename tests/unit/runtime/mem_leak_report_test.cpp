// RT-INV-MEM-03 (SL-ARCH-003 ARCH3-R044, ADR-0023): a resource destroyed with live blocks reports
// each block's tag, size and (Debug) site. The tracking_resource sits on a monotonic buffer that
// outlives it, so the leaked blocks are reclaimed with the buffer (LeakSanitizer stays quiet). A
// memory_sink on the default logger captures the runtime.mem error events of the destructor.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

// Attaches a memory_sink to the default logger for the scope of the object.
class captured_log {
public:
  captured_log() : sink_(std::make_shared<sl::log::memory_sink>(64)) {
    sl::log::default_logger().add_sink(sink_);
  }
  captured_log(const captured_log &) = delete;
  captured_log(captured_log &&) = delete;
  captured_log &operator=(const captured_log &) = delete;
  captured_log &operator=(captured_log &&) = delete;
  ~captured_log() { sl::log::default_logger().remove_sink(sink_.get()); }

  [[nodiscard]] std::vector<sl::log::memory_sink::record> runtime_mem_errors() const {
    std::vector<sl::log::memory_sink::record> out;
    for (auto &r : sink_->records()) {
      if (r.system == "runtime.mem" && r.level == sl::log::level::error) {
        out.push_back(std::move(r));
      }
    }
    return out;
  }

private:
  std::shared_ptr<sl::log::memory_sink> sink_;
};

std::string text_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v)
                                                                 : std::string();
}

std::int64_t int_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

bool ends_with_test_file(std::string_view file) {
  return file.ends_with("mem_leak_report_test.cpp");
}

} // namespace

TEST_CASE("RT-INV-MEM-03: a resource destroyed with live blocks reports each block's tag, size "
          "and site") {
  if (!mem::debug_memory) {
    MESSAGE("skipped: the site table exists in Debug and sanitizer builds only");
    return;
  }
  mem::tag_registry registry;
  const auto chunks = registry.add("ecs.chunks");
  const auto events = registry.add("events");
  REQUIRE(chunks.has_value());
  REQUIRE(events.has_value());
  REQUIRE(registry.freeze().has_value());

  std::vector<std::byte> buffer(std::size_t{1} << 16U);
  std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size(),
                                            std::pmr::null_memory_resource());
  captured_log log;
  std::uint32_t line_a = 0;
  std::uint32_t line_b = 0;
  std::uint32_t line_c = 0;
  {
    mem::tracking_resource r("leaky", *chunks, &registry, &arena);
    // each site is the line after the one that records it
    line_a = std::source_location::current().line() + 1;
    auto a = r.try_allocate(100, 8, *events);
    line_b = std::source_location::current().line() + 1;
    auto b = r.try_allocate(32, 64, *chunks);
    line_c = std::source_location::current().line() + 1;
    auto c = r.try_allocate(7, 1, *chunks);
    auto d = r.try_allocate(500, 16, *events);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(d.has_value());
    r.release(*d, 500, 16);

    const std::vector<mem::leak_record> leaks = r.leaks();
    REQUIRE(leaks.size() == 3);
    // sorted by (tag name, file, line, size): ecs.chunks before events, b before c
    CHECK(leaks[0].tag == *chunks);
    CHECK(leaks[0].size == 32);
    CHECK(leaks[0].line == line_b);
    CHECK(leaks[1].tag == *chunks);
    CHECK(leaks[1].size == 7);
    CHECK(leaks[1].line == line_c);
    CHECK(leaks[2].tag == *events);
    CHECK(leaks[2].size == 100);
    CHECK(leaks[2].line == line_a);
    for (const mem::leak_record &l : leaks) {
      CHECK(ends_with_test_file(l.file));
    }
    CHECK(log.runtime_mem_errors().empty());
  }

  const auto reported = log.runtime_mem_errors();
  REQUIRE(reported.size() == 3);
  CHECK(text_field(reported[0], "tag") == "ecs.chunks");
  CHECK(int_field(reported[0], "size") == 32);
  CHECK(int_field(reported[0], "line") == line_b);
  CHECK(text_field(reported[1], "tag") == "ecs.chunks");
  CHECK(int_field(reported[1], "size") == 7);
  CHECK(int_field(reported[1], "line") == line_c);
  CHECK(text_field(reported[2], "tag") == "events");
  CHECK(int_field(reported[2], "size") == 100);
  CHECK(int_field(reported[2], "line") == line_a);
  for (const auto &rec : reported) {
    CHECK(text_field(rec, "resource") == "leaky");
    CHECK(ends_with_test_file(text_field(rec, "file")));
  }
}

TEST_CASE("RT-INV-MEM-03: a resource destroyed with every block released reports nothing") {
  captured_log log;
  {
    mem::tracking_resource r("clean");
    auto a = r.try_allocate(64, 16, r.default_tag());
    REQUIRE(a.has_value());
    r.release(*a, 64, 16);
    CHECK(r.leaks().empty());
  }
  CHECK(log.runtime_mem_errors().empty());
}
