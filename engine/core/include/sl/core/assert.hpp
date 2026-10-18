#pragma once

// Assertions that produce machine-readable reports (WO-030, SL-EP-056, ADR-0009).
//
//   SL_ASSERT(cond, msg)        checked in every build
//   SL_VERIFY(cond, msg)        checked in every build; use it when `cond` has side effects that
//   must
//                               run even if SL_ASSERT is ever compiled out
//   SL_DEBUG_ASSERT(cond, msg)  checked only when NDEBUG is not defined (Debug); never evaluated
//                               otherwise
//
// On failure: a fatal event (system "assert") goes to the default logger with the expression, the
// message, file:line:function and, when the toolchain provides std::stacktrace (SL_HAS_STACKTRACE,
// linked with stdc++exp), the stack trace; then the assert handler runs. The default handler writes
// a JSON report {expression, message, where, stacktrace[]} to the path in the environment variable
// SL_ASSERT_REPORT_JSON (the value "1" means ./assert-report.json), flushes the logger and calls
// std::abort(). A test installs its own handler (scoped_assert_handler) to capture the report; when
// that handler returns, execution continues after the failed assertion.

#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"

namespace sl {

struct assert_report {
  std::string_view expression;
  std::string_view message;
  std::source_location where;
  /// One entry per frame, innermost first ("function at file:line"); empty without stacktrace.
  std::vector<std::string> stacktrace;
};

using assert_handler = std::function<void(const assert_report &)>;

/// Installs `handler` (an empty function restores the default) and returns the previous one.
assert_handler set_assert_handler(assert_handler handler);

/// RAII: installs a handler for its lifetime, then restores the previous one.
class scoped_assert_handler {
public:
  explicit scoped_assert_handler(assert_handler handler)
      : previous_(set_assert_handler(std::move(handler))) {}
  ~scoped_assert_handler() { set_assert_handler(std::move(previous_)); }
  scoped_assert_handler(const scoped_assert_handler &) = delete;
  scoped_assert_handler &operator=(const scoped_assert_handler &) = delete;
  scoped_assert_handler(scoped_assert_handler &&) = delete;
  scoped_assert_handler &operator=(scoped_assert_handler &&) = delete;

private:
  assert_handler previous_;
};

/// The default handler: report file (when SL_ASSERT_REPORT_JSON is set), flush, std::abort().
[[noreturn]] void default_assert_handler(const assert_report &report) noexcept;

/// True when assertion reports carry a stack trace in this build.
[[nodiscard]] bool assert_stacktrace_enabled() noexcept;

/// The report as one JSON object:
/// {"expression":..,"message":..,"where":{"file":..,"line":..,"function":..},"stacktrace":[..]}.
[[nodiscard]] std::string format_assert_report_json(const assert_report &report);

/// Writes format_assert_report_json(report) and a newline to `path` (replacing the file).
[[nodiscard]] result<void> write_assert_report(const assert_report &report, const char *path);

namespace detail {
/// Called by the macros on failure: logs, then runs the handler (which aborts unless overridden).
void assert_fail(std::string_view expression, std::string_view message,
                 std::source_location where) noexcept;
} // namespace detail

} // namespace sl

#define SL_ASSERT(cond, msg)                                                                       \
  do {                                                                                             \
    if (!(cond)) [[unlikely]] {                                                                    \
      ::sl::detail::assert_fail(#cond, (msg), ::std::source_location::current());                  \
    }                                                                                              \
  } while (false)

#define SL_VERIFY(cond, msg) SL_ASSERT(cond, msg)

#if defined(NDEBUG)
#define SL_DEBUG_ASSERT(cond, msg)                                                                 \
  do {                                                                                             \
    static_cast<void>(sizeof(!(cond)));                                                            \
    static_cast<void>(sizeof(msg));                                                                \
  } while (false)
#else
#define SL_DEBUG_ASSERT(cond, msg) SL_ASSERT(cond, msg)
#endif
