#include "sl/core/assert.hpp"

#include "sl/core/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <vector>
#include <version>

// std::stacktrace needs the library feature AND the configure-time link check (SL_HAS_STACKTRACE,
// engine/core/CMakeLists.txt: libstdc++ keeps it in stdc++exp).
#if defined(SL_HAS_STACKTRACE) && defined(__cpp_lib_stacktrace)
#include <stacktrace>
#define SL_ASSERT_USE_STACKTRACE 1
#endif

namespace sl {

namespace {

// The installed handler, behind one accessor instead of mutable globals.
struct handler_state {
  std::mutex mutex;
  assert_handler handler;
};

handler_state &handlers() noexcept {
  static handler_state instance;
  return instance;
}

// Innermost frame first. Noexcept: when capturing or formatting fails (allocation), the report
// goes out with no trace, as in a build without std::stacktrace.
std::vector<std::string> capture_stacktrace() noexcept {
  std::vector<std::string> frames;
#ifdef SL_ASSERT_USE_STACKTRACE
  try {
    const std::stacktrace trace = std::stacktrace::current(1);
    frames.reserve(trace.size());
    for (const std::stacktrace_entry &entry : trace) {
      std::string text = entry.description();
      if (text.empty()) {
        text = "??";
      }
      if (const std::string file = entry.source_file(); !file.empty()) {
        std::format_to(std::back_inserter(text), " at {}:{}", file, entry.source_line());
      }
      frames.push_back(std::move(text));
    }
  } catch (...) {
    frames.clear(); // a partial trace would mislead: report none
  }
#endif
  return frames;
}

// The fatal "assert" event to the default logger. Noexcept: when joining the frames fails, the
// event goes out without the stacktrace field.
void log_assert_event(const assert_report &report) noexcept {
  std::string joined;
  try {
    for (const std::string &frame : report.stacktrace) {
      if (!joined.empty()) {
        joined += '\n';
      }
      joined += frame;
    }
  } catch (...) {
    joined.clear();
  }
  log::emit(log::level::fatal, "assert", report.message,
            {{"expression", report.expression},
             {"function", report.where.function_name()},
             {"frames", report.stacktrace.size()},
             {"stacktrace", std::string_view{joined}}},
            report.where);
  log::default_logger().flush();
}

// The report file and the stderr line of default_assert_handler (may throw: formatting,
// allocation).
void report_assert(const assert_report &report) {
  if (const char *env = std::getenv("SL_ASSERT_REPORT_JSON"); env != nullptr && *env != '\0') {
    const char *path = std::string_view{env} == "1" ? "assert-report.json" : env;
    if (const result<void> written = write_assert_report(report, path); !written) {
      std::println(stderr, "sl: {}", describe(written.error()));
    }
  }
  std::println(stderr, "sl: assertion failed: {} ({}) at {}:{}", report.expression, report.message,
               report.where.file_name(), report.where.line());
}

struct file_closer {
  void operator()(std::FILE *f) const noexcept { std::fclose(f); }
};

} // namespace

assert_handler set_assert_handler(assert_handler handler) {
  handler_state &st = handlers();
  const std::scoped_lock lock(st.mutex);
  assert_handler previous = std::move(st.handler);
  st.handler = std::move(handler);
  return previous;
}

bool assert_stacktrace_enabled() noexcept {
#ifdef SL_ASSERT_USE_STACKTRACE
  return true;
#else
  return false;
#endif
}

std::string format_assert_report_json(const assert_report &report) {
  std::string out = R"({"expression":)";
  log::append_json_string(out, report.expression);
  out += R"(,"message":)";
  log::append_json_string(out, report.message);
  out += R"(,"where":{"file":)";
  log::append_json_string(out, report.where.file_name());
  std::format_to(std::back_inserter(out), R"(,"line":{},"function":)", report.where.line());
  log::append_json_string(out, report.where.function_name());
  out += R"(},"stacktrace":[)";
  for (std::size_t i = 0; i < report.stacktrace.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    log::append_json_string(out, report.stacktrace[i]);
  }
  out += "]}";
  return out;
}

result<void> write_assert_report(const assert_report &report, const char *path) {
  if (path == nullptr || *path == '\0') {
    return make_error(errc::invalid_argument, "assert report path is empty");
  }
  const std::unique_ptr<std::FILE, file_closer> file{std::fopen(path, "wb")};
  if (!file) {
    return make_error(errc::io_error, std::format("cannot open assert report '{}'", path));
  }
  std::string text = format_assert_report_json(report);
  text += '\n';
  if (std::fwrite(text.data(), 1, text.size(), file.get()) != text.size() ||
      std::fflush(file.get()) != 0) {
    return make_error(errc::io_error, std::format("cannot write assert report '{}'", path));
  }
  return {};
}

void default_assert_handler(const assert_report &report) noexcept {
  try {
    report_assert(report);
  } catch (...) {
    // Formatting or the report file failed; the process still aborts with a fixed line.
    std::fputs("sl: assertion failed (report could not be written)\n", stderr);
  }
  log::default_logger().flush();
  std::fflush(nullptr);
  std::abort();
}

void detail::assert_fail(std::string_view expression, std::string_view message,
                         std::source_location where) noexcept {
  const assert_report report{
      .expression = expression,
      .message = message,
      .where = where,
      .stacktrace = capture_stacktrace(),
  };
  log_assert_event(report);

  // A handler that throws (or a failed copy of it) cannot unwind through this noexcept function:
  // the default handler then reports and aborts.
  try {
    assert_handler handler;
    {
      handler_state &st = handlers();
      const std::scoped_lock lock(st.mutex);
      handler = st.handler;
    }
    if (handler) {
      handler(report);
      return;
    }
  } catch (...) {
    default_assert_handler(report);
  }
  default_assert_handler(report);
}

} // namespace sl
