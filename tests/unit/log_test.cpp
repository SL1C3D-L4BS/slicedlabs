#include "json_check.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <sl/core/log.hpp>
#include <sl/core/version.hpp>

namespace {

using sl::log::correlation;
using sl::log::event;
using sl::log::field;
using sl::log::level;

// A fixed event: default source_location (empty file, line 0) and the epoch, so output is exact.
event fixed_event(std::span<const field> fields) {
  return event{
      .level = level::info,
      .system = "core",
      .message = "hello",
      .fields = fields,
      .correlation = correlation{.id = 7, .scope = "entity:42", .tick = 9},
      .where = {},
      .ts = std::chrono::system_clock::time_point{},
  };
}

std::string json_of(std::initializer_list<field> fields) {
  return sl::log::json_sink::format(fixed_event({fields.begin(), fields.size()}));
}

// The text of the "fields":{...} object of a json_sink line.
std::string_view fields_json(std::string_view line) {
  const std::size_t start = line.find(R"("fields":)") + 9;
  const std::size_t end = line.find(R"(,"correlation":)");
  return line.substr(start, end - start);
}

} // namespace

TEST_CASE("log levels are ordered and named") {
  CHECK(level::trace < level::debug);
  CHECK(level::debug < level::info);
  CHECK(level::info < level::warn);
  CHECK(level::warn < level::error);
  CHECK(level::error < level::fatal);
  CHECK(sl::log::to_string(level::trace) == "trace");
  CHECK(sl::log::to_string(level::warn) == "warn");
  CHECK(sl::log::to_string(level::fatal) == "fatal");
}

TEST_CASE("logger drops events below its minimum level") {
  sl::log::logger lg;
  auto mem = std::make_shared<sl::log::memory_sink>(16);
  lg.add_sink(mem);
  CHECK(lg.min_level() == level::info);
  CHECK_FALSE(lg.enabled(level::debug));
  CHECK(lg.enabled(level::warn));

  for (const level l :
       {level::trace, level::debug, level::info, level::warn, level::error, level::fatal}) {
    sl::log::emit(lg, l, "test", "msg");
  }
  CHECK(mem->total() == 4);
  CHECK(mem->records().front().level == level::info);

  lg.set_min_level(level::error);
  sl::log::emit(lg, level::warn, "test", "dropped");
  sl::log::emit(lg, level::error, "test", "kept");
  CHECK(mem->total() == 5);
  CHECK(mem->records().back().message == "kept");

  lg.set_min_level(level::trace);
  sl::log::emit(lg, level::trace, "test", "trace kept");
  CHECK(mem->total() == 6);
}

TEST_CASE("logger sinks can be added, removed and cleared") {
  sl::log::logger lg;
  auto a = std::make_shared<sl::log::memory_sink>(4);
  auto b = std::make_shared<sl::log::memory_sink>(4);
  lg.add_sink(a);
  lg.add_sink(b);
  lg.add_sink(nullptr);
  CHECK(lg.sink_count() == 2);
  sl::log::emit(lg, level::info, "test", "both");
  CHECK(lg.remove_sink(a.get()));
  CHECK_FALSE(lg.remove_sink(a.get()));
  sl::log::emit(lg, level::info, "test", "b only");
  CHECK(a->total() == 1);
  CHECK(b->total() == 2);
  lg.clear_sinks();
  CHECK(lg.sink_count() == 0);
  sl::log::emit(lg, level::info, "test", "nobody");
  CHECK(b->total() == 2);
}

TEST_CASE("field values keep their type") {
  const std::string owned = "owned";
  const field i{"i", 3};
  const field u{"u", 7U};
  const field z{"z", std::size_t{42}};
  const field d{"d", 0.25};
  const field f{"f", 1.5F};
  const field b{"b", true};
  const field c{"c", "literal"};
  const field s{"s", std::string_view{"view"}};
  const field o{"o", owned};
  const field big{"big", std::numeric_limits<std::uint64_t>::max()};

  CHECK(std::get<std::int64_t>(i.value) == 3);
  CHECK(std::get<std::int64_t>(u.value) == 7);
  CHECK(std::get<std::int64_t>(z.value) == 42);
  CHECK(std::get<double>(d.value) == doctest::Approx(0.25));
  CHECK(std::get<double>(f.value) == doctest::Approx(1.5));
  CHECK(std::get<bool>(b.value));
  CHECK(std::get<std::string_view>(c.value) == "literal");
  CHECK(std::get<std::string_view>(s.value) == "view");
  CHECK(std::get<std::string_view>(o.value) == "owned");
  CHECK(std::get<std::int64_t>(big.value) == -1); // documented: unsigned above INT64_MAX wraps
}

TEST_CASE("json_sink writes the exact documented object") {
  const std::string line =
      json_of({{"n", 3}, {"x", 0.5}, {"ok", true}, {"name", "a\"b"}, {"neg", -12}});
  CHECK(line == R"({"schema":"sl-log-event/1","level":"info","system":"core","message":"hello",)"
                R"("fields":{"n":3,"x":0.5,"ok":true,"name":"a\"b","neg":-12},)"
                R"("correlation":{"id":7,"scope":"entity:42","tick":9},)"
                R"("where":{"file":"","line":0,"function":""},"ts":"1970-01-01T00:00:00.000Z"})");
  CHECK(sl::test::json_valid(line));
  CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("json_sink serialises every field variant") {
  CHECK(fields_json(json_of({{"a", 3.0}})) == R"({"a":3.0})");
  CHECK(fields_json(json_of({{"a", 1e20}})) == R"({"a":1e+20})");
  CHECK(fields_json(json_of({{"a", -0.125}})) == R"({"a":-0.125})");
  CHECK(fields_json(json_of({{"a", std::nan("")}})) == R"({"a":null})");
  CHECK(fields_json(json_of({{"a", std::numeric_limits<double>::infinity()}})) == R"({"a":null})");
  CHECK(fields_json(json_of({{"a", std::numeric_limits<std::int64_t>::min()}})) ==
        R"({"a":-9223372036854775808})");
  CHECK(fields_json(json_of({{"a", std::numeric_limits<std::int64_t>::max()}})) ==
        R"({"a":9223372036854775807})");
  CHECK(fields_json(json_of({{"t", true}, {"f", false}})) == R"({"t":true,"f":false})");
  CHECK(fields_json(json_of({})) == "{}");
  CHECK(fields_json(json_of({{"k", 1}, {"k", 2}})) == R"({"k":1})"); // first value of a key wins
}

TEST_CASE("json_sink escapes strings and repairs invalid UTF-8") {
  const std::string_view nasty = "q\" b\\ nl\n cr\r tab\t bell\x07 del\x7f";
  const std::string line = json_of({{"s", nasty}, {"k\"ey", 1}});
  CHECK(sl::test::json_valid(line));
  CHECK(fields_json(line) == R"({"s":"q\" b\\ nl\n cr\r tab\t bell\u0007 del)"
                             "\x7f"
                             R"(","k\"ey":1})");

  // Valid multi-byte UTF-8 passes through; invalid bytes become \ufffd and the line stays valid.
  const std::string utf8 = json_of({{"ok", "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae"}});
  CHECK(fields_json(utf8) == "{\"ok\":\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae\"}");
  const std::string bad = json_of({{"bad", "a\xff b\xc3 c\xed\xa0\x80 d\xe2\x82"}});
  CHECK(fields_json(bad) == R"({"bad":"a\ufffd b\ufffd c\ufffd\ufffd\ufffd d\ufffd\ufffd"})");
  CHECK(sl::test::json_valid(bad));
}

TEST_CASE("json_sink lines parse back as JSON with a real source location and time") {
  sl::log::logger lg;
  auto mem = std::make_shared<sl::log::memory_sink>(1);
  lg.add_sink(mem);
  const std::vector<field> fields{{"frame", 12}, {"ms", 16.6}, {"path", "C:\\tmp\\a b.txt"}};
  const event e{.level = level::warn,
                .system = "render",
                .message = "slow frame",
                .fields = fields,
                .correlation = sl::log::current_correlation(),
                .where = std::source_location::current(),
                .ts = std::chrono::system_clock::now()};
  const std::string line = sl::log::json_sink::format(e);
  CHECK(sl::test::json_valid(line));
  CHECK(line.find(R"("level":"warn")") != std::string::npos);
  CHECK(line.find("log_test.cpp") != std::string::npos);
  CHECK(line.find(R"("path":"C:\\tmp\\a b.txt")") != std::string::npos);
  const std::size_t ts = line.find(R"("ts":")");
  REQUIRE(ts != std::string::npos);
  const std::string_view stamp = std::string_view{line}.substr(ts + 6, 24);
  CHECK(stamp.size() == 24);
  CHECK(stamp[4] == '-');
  CHECK(stamp[10] == 'T');
  CHECK(stamp[19] == '.');
  CHECK(stamp[23] == 'Z');
}

TEST_CASE("stderr_sink renders one human-readable line") {
  const std::vector<field> fields{{"n", 3}, {"name", "x"}};
  const std::string text = sl::log::stderr_sink::format(fixed_event(fields));
  CHECK(text == R"(1970-01-01T00:00:00.000Z INFO  [core] hello n=3 name="x" )"
                R"({scope=entity:42 id=7 tick=9} :0)");
}

TEST_CASE("correlation defaults to the process-wide scope tag") {
  const correlation c = sl::log::current_correlation();
  CHECK(c.id == 0);
  CHECK(c.scope == "process");
  CHECK(c.tick == 0);
  sl::log::set_process_scope("sl_tests");
  CHECK(sl::log::current_correlation().scope == "sl_tests");
  sl::log::set_process_scope("process");
  CHECK(sl::log::process_scope() == "process");
}

TEST_CASE("correlation scopes nest and restore") {
  sl::log::logger lg;
  auto mem = std::make_shared<sl::log::memory_sink>(8);
  lg.add_sink(mem);
  {
    const sl::log::correlation_scope outer("job:load", 5);
    CHECK(outer.get().id != 0);
    sl::log::emit(lg, level::info, "test", "outer");
    {
      const sl::log::correlation_scope inner(correlation{.id = 99, .scope = "entity:7", .tick = 6});
      sl::log::emit(lg, level::info, "test", "inner");
      CHECK(sl::log::current_correlation().id == 99);
    }
    CHECK(sl::log::current_correlation().id == outer.get().id);
    sl::log::emit(lg, level::info, "test", "outer again");
    const sl::log::correlation_scope sibling("job:save");
    CHECK(sibling.get().id != outer.get().id);
  }
  sl::log::emit(lg, level::info, "test", "process");

  const auto recs = mem->records();
  REQUIRE(recs.size() == 4);
  CHECK(recs[0].scope == "job:load");
  CHECK(recs[0].tick == 5);
  CHECK(recs[1].scope == "entity:7");
  CHECK(recs[1].correlation_id == 99);
  CHECK(recs[2].scope == "job:load");
  CHECK(recs[2].correlation_id == recs[0].correlation_id);
  CHECK(recs[3].scope == "process");
  CHECK(recs[3].correlation_id == 0);
}

TEST_CASE("correlation is per thread") {
  const sl::log::correlation_scope mine("main-thread");
  correlation seen{};
  std::thread worker([&seen] { seen = sl::log::current_correlation(); });
  worker.join();
  CHECK(seen.scope == "process");
  CHECK(sl::log::current_correlation().scope == "main-thread");
}

TEST_CASE("memory_sink keeps the last N events and wraps around") {
  sl::log::logger lg;
  lg.set_min_level(level::trace);
  auto mem = std::make_shared<sl::log::memory_sink>(3);
  lg.add_sink(mem);
  for (int i = 0; i < 5; ++i) {
    sl::log::emit(lg, level::debug, "ring", "event", {{"i", i}});
  }
  CHECK(mem->capacity() == 3);
  CHECK(mem->size() == 3);
  CHECK(mem->total() == 5);
  const auto recs = mem->records();
  REQUIRE(recs.size() == 3);
  for (std::size_t k = 0; k < recs.size(); ++k) {
    const auto *v = recs[k].find("i");
    REQUIRE(v != nullptr);
    CHECK(std::get<std::int64_t>(*v) == static_cast<std::int64_t>(k) + 2);
  }
  CHECK(recs[0].find("missing") == nullptr);
  mem->clear();
  CHECK(mem->size() == 0);
  CHECK(mem->total() == 0);
  CHECK(sl::log::memory_sink{0}.capacity() == 1);
}

TEST_CASE("memory_sink deep-copies the event") {
  sl::log::logger lg;
  auto mem = std::make_shared<sl::log::memory_sink>(2);
  lg.add_sink(mem);
  {
    std::string system = "temp-system";
    std::string value = "temp-value";
    sl::log::emit(lg, level::info, system, "copy", {{"v", std::string_view{value}}});
    system.assign("overwritten");
    value.assign("overwritten");
  }
  const auto recs = mem->records();
  REQUIRE(recs.size() == 1);
  CHECK(recs[0].system == "temp-system");
  CHECK(std::get<std::string>(*recs[0].find("v")) == "temp-value");
  CHECK(recs[0].file.find("log_test.cpp") != std::string::npos);
  CHECK(recs[0].line > 0);
}

TEST_CASE("logger is safe to use from several threads") {
  sl::log::logger lg;
  auto mem = std::make_shared<sl::log::memory_sink>(64);
  lg.add_sink(mem);
  constexpr int thread_count = 4;
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&lg, t] {
      const sl::log::correlation_scope scope("worker");
      for (int i = 0; i < 250; ++i) {
        sl::log::emit(lg, level::info, "mt", "tick", {{"t", t}, {"i", i}});
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  CHECK(mem->total() == 1000);
  CHECK(mem->size() == 64);
}

TEST_CASE("SL_LOG_* macros and log_build_info reach the default logger") {
  auto &lg = sl::log::default_logger();
  auto mem = std::make_shared<sl::log::memory_sink>(8);
  lg.add_sink(mem);
  const level saved = lg.min_level();
  lg.set_min_level(level::info);
  SL_LOG_DEBUG("macro", "filtered");
  SL_LOG_INFO("macro", "no fields");
  SL_LOG_WARN("macro", "with fields", {"count", 2}, {"ratio", 0.5});
  sl::core::log_build_info();
  lg.set_min_level(saved);
  lg.remove_sink(mem.get());

  const auto recs = mem->records();
  REQUIRE(recs.size() == 3);
  CHECK(recs[0].message == "no fields");
  CHECK(recs[0].fields.empty());
  CHECK(recs[1].level == level::warn);
  CHECK(recs[1].fields.size() == 2);
  CHECK(recs[2].system == "core");
  CHECK(recs[2].message == "build info");
  CHECK(recs[2].fields.size() == 8);
  const auto *compiler = recs[2].find("compiler");
  REQUIRE(compiler != nullptr);
  CHECK(std::get<std::string>(*compiler) == sl::core::build_info().compiler);
}

namespace {

// A sink whose every write fails inside it, the way an allocation failure would.
class failing_sink final : public sl::log::sink {
public:
  void write(const event & /*e*/) noexcept override {
    try {
      throw std::runtime_error("planted write failure");
    } catch (...) {
      record_failure();
    }
  }
};

} // namespace

TEST_CASE("a write failure is caught at the sink boundary and counted, not propagated") {
  sl::log::logger lg;
  auto failing = std::make_shared<failing_sink>();
  auto mem = std::make_shared<sl::log::memory_sink>(4);
  lg.add_sink(failing);
  lg.add_sink(mem);
  sl::log::emit(lg, level::info, "core", "one");
  sl::log::emit(lg, level::warn, "core", "two");
  CHECK(failing->failures() == 2);
  CHECK(mem->failures() == 0);
  CHECK(mem->total() == 2);
}
