// Test-run logging setup (WO-030). Before any test runs, the default logger loses its stderr sink
// (quiet test output) and accepts every level. When SL_LOG_JSON_PATH names a file, a json_sink
// writes every default-logger event of the run there (`just log-schema-check` validates it against
// schemas/sl-log-event.schema.json). The corpus test below makes sure each level, field type and
// correlation form appears in that file.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <print>

#include <sl/core/log.hpp>
#include <sl/core/version.hpp>

namespace {

struct file_closer {
  void operator()(std::FILE *f) const noexcept { std::fclose(f); }
};

class json_log_fixture {
public:
  // noexcept: this runs during static initialization, where an exception cannot be caught; a
  // setup failure is reported on stderr and the run continues without the JSON file.
  json_log_fixture() noexcept {
    try {
      attach();
    } catch (...) {
      std::fputs("log fixture: setup failed, no SL_LOG_JSON_PATH output\n", stderr);
    }
  }
  ~json_log_fixture() {
    if (sink_) {
      sl::log::default_logger().remove_sink(sink_.get());
    }
  }
  json_log_fixture(const json_log_fixture &) = delete;
  json_log_fixture &operator=(const json_log_fixture &) = delete;
  json_log_fixture(json_log_fixture &&) = delete;
  json_log_fixture &operator=(json_log_fixture &&) = delete;

private:
  void attach() {
    sl::log::logger &lg = sl::log::default_logger();
    lg.clear_sinks();
    lg.set_min_level(sl::log::level::trace);
    const char *path = std::getenv("SL_LOG_JSON_PATH");
    if (path == nullptr || *path == '\0') {
      return;
    }
    // Truncate, then append: a test that adds raw lines through its own append handle cannot be
    // overwritten by this one.
    file_.reset(std::fopen(path, "w"));
    file_.reset(file_ ? std::fopen(path, "a") : nullptr);
    if (!file_) {
      std::println(stderr, "log fixture: cannot open SL_LOG_JSON_PATH={}", path);
      return;
    }
    sink_ = std::make_shared<sl::log::json_sink>(file_.get());
    lg.add_sink(sink_);
  }

  std::unique_ptr<std::FILE, file_closer> file_;
  std::shared_ptr<sl::log::json_sink> sink_;
};

const json_log_fixture g_json_log_fixture;

} // namespace

TEST_CASE("log corpus: every level, field type and correlation form reaches the default logger") {
  auto &lg = sl::log::default_logger();
  auto mem = std::make_shared<sl::log::memory_sink>(32);
  lg.add_sink(mem);

  SL_LOG_TRACE("corpus", "trace event", {"int", -3});
  SL_LOG_DEBUG("corpus", "debug event", {"double", 2.5}, {"nan", std::nan("")});
  SL_LOG_INFO("corpus", "info event", {"bool", true}, {"text", "quote \" backslash \\ tab \t"});
  SL_LOG_WARN("corpus", "warn event", {"max", std::numeric_limits<std::int64_t>::max()});
  {
    const sl::log::correlation_scope job("job:corpus", 17);
    SL_LOG_ERROR("corpus", "error event in a job scope", {"utf8", "caf\xc3\xa9"});
    const sl::log::correlation_scope entity(
        sl::log::correlation{.id = 4242, .scope = "entity:7", .tick = 18});
    SL_LOG_FATAL("corpus", "fatal event in an entity scope (not an abort)");
  }
  sl::log::set_process_scope("sl_tests");
  SL_LOG_INFO("corpus", "custom process scope");
  sl::log::set_process_scope("process");
  sl::core::log_build_info();
  lg.remove_sink(mem.get());

  CHECK(mem->total() == 8);
  const auto recs = mem->records();
  REQUIRE(recs.size() == 8);
  CHECK(recs[0].level == sl::log::level::trace);
  CHECK(recs[5].level == sl::log::level::fatal);
  CHECK(recs[5].correlation_id == 4242);
  CHECK(recs[6].scope == "sl_tests");
  CHECK(recs[7].message == "build info");
}
