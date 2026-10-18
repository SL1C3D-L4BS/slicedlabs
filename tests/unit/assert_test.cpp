#include "json_check.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sl/core/assert.hpp>
#include <sl/core/log.hpp>

namespace {

// A copy of the report: its views only live during the handler call.
struct captured {
  std::string expression;
  std::string message;
  std::string file;
  std::string function;
  unsigned line = 0;
  std::vector<std::string> stacktrace;
  std::string json;
  int calls = 0;
};

sl::assert_handler capture_into(captured &out) {
  return [&out](const sl::assert_report &r) {
    out.expression = r.expression;
    out.message = r.message;
    out.file = r.where.file_name();
    out.function = r.where.function_name();
    out.line = r.where.line();
    out.stacktrace = r.stacktrace;
    out.json = sl::format_assert_report_json(r);
    ++out.calls;
  };
}

} // namespace

TEST_CASE("SL_ASSERT failure reaches an overriding handler with a full report") {
  captured got;
  const sl::scoped_assert_handler guard(capture_into(got));
  const int answer = 41;
  const unsigned line = __LINE__ + 1;
  SL_ASSERT(answer == 42, "the answer must be 42");
  REQUIRE(got.calls == 1);
  CHECK(got.expression == "answer == 42");
  CHECK(got.message == "the answer must be 42");
  CHECK(got.file.ends_with("assert_test.cpp"));
  CHECK(got.line == line);
  CHECK(got.function.find("DOCTEST") != std::string::npos);
  CHECK(sl::test::json_valid(got.json));
  CHECK(got.json.starts_with(R"({"expression":"answer == 42","message":"the answer must be 42",)"));
  CHECK(got.json.find(R"("stacktrace":[)") != std::string::npos);
  if (sl::assert_stacktrace_enabled()) {
    CHECK_FALSE(got.stacktrace.empty());
  } else {
    CHECK(got.stacktrace.empty());
  }
}

TEST_CASE("a passing assertion does nothing; SL_VERIFY evaluates once") {
  captured got;
  const sl::scoped_assert_handler guard(capture_into(got));
  int evaluated = 0;
  const auto side_effect = [&evaluated] {
    ++evaluated;
    return true;
  };
  SL_ASSERT(1 + 1 == 2, "arithmetic");
  SL_VERIFY(side_effect(), "side effect");
  CHECK(got.calls == 0);
  CHECK(evaluated == 1);
}

TEST_CASE("SL_DEBUG_ASSERT is checked only without NDEBUG") {
  captured got;
  const sl::scoped_assert_handler guard(capture_into(got));
  int evaluated = 0;
  const auto fails = [&evaluated] {
    ++evaluated;
    return false;
  };
  SL_DEBUG_ASSERT(fails(), "debug only");
#ifdef NDEBUG
  CHECK(evaluated == 0);
  CHECK(got.calls == 0);
#else
  CHECK(evaluated == 1);
  CHECK(got.calls == 1);
  CHECK(got.message == "debug only");
#endif
}

TEST_CASE("an assertion failure emits one fatal event with the expression and frames") {
  captured got;
  const sl::scoped_assert_handler guard(capture_into(got));
  auto &lg = sl::log::default_logger();
  auto mem = std::make_shared<sl::log::memory_sink>(4);
  lg.add_sink(mem);
  SL_ASSERT(mem == nullptr, "planted failure");
  lg.remove_sink(mem.get());

  const auto recs = mem->records();
  REQUIRE(recs.size() == 1);
  CHECK(recs[0].level == sl::log::level::fatal);
  CHECK(recs[0].system == "assert");
  CHECK(recs[0].message == "planted failure");
  CHECK(recs[0].file.ends_with("assert_test.cpp"));
  CHECK(std::get<std::string>(*recs[0].find("expression")) == "mem == nullptr");
  CHECK(std::get<std::int64_t>(*recs[0].find("frames")) ==
        static_cast<std::int64_t>(got.stacktrace.size()));
  CHECK(recs[0].find("stacktrace") != nullptr);
}

TEST_CASE("scoped_assert_handler nests and restores the previous handler") {
  captured outer;
  captured inner;
  const sl::scoped_assert_handler a(capture_into(outer));
  {
    const sl::scoped_assert_handler b(capture_into(inner));
    SL_ASSERT(false, "to inner");
  }
  SL_ASSERT(false, "to outer");
  CHECK(inner.calls == 1);
  CHECK(inner.message == "to inner");
  CHECK(outer.calls == 1);
  CHECK(outer.message == "to outer");
}

TEST_CASE("write_assert_report writes the JSON report file") {
  captured got;
  std::optional<sl::assert_report> copy;
  const sl::scoped_assert_handler guard([&](const sl::assert_report &r) {
    // the test working directory (ctest runs in the build tree): /tmp is read-only inside the
    // factory verify jail
    const auto path = std::filesystem::current_path() / "sl_assert_report_test.json";
    const sl::result<void> written = sl::write_assert_report(r, path.string().c_str());
    REQUIRE(written.has_value());
    std::ifstream in(path);
    got.json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    std::filesystem::remove(path);
    ++got.calls;
  });
  SL_ASSERT(got.calls < 0, "report \"quoted\"\n");
  REQUIRE(got.calls == 1);
  CHECK(got.json.ends_with("}\n"));
  CHECK(sl::test::json_valid(got.json));
  CHECK(got.json.find(R"("message":"report \"quoted\"\n")") != std::string::npos);
  CHECK(got.json.find(R"("line":)") != std::string::npos);

  const sl::assert_report empty{};
  const auto no_path = sl::write_assert_report(empty, "");
  REQUIRE_FALSE(no_path.has_value());
  CHECK(no_path.error().code == sl::errc::invalid_argument);
  const auto bad_dir = sl::write_assert_report(empty, "/nonexistent-sl-dir/report.json");
  REQUIRE_FALSE(bad_dir.has_value());
  CHECK(bad_dir.error().code == sl::errc::io_error);
}
