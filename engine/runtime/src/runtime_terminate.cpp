// install_terminate_report (WO-096, SL-ARCH-003 ARCH3-R015, ADR-0009): for hosts only, the library
// never calls it. The handler writes one fatal sl::log event (system runtime.terminate) and the
// assert report through sl::default_assert_handler, which aborts.

#include "sl/runtime/runtime.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"

#include <exception>
#include <source_location>

namespace sl::runtime {

namespace {

void terminate_report() noexcept {
  sl::log::emit(sl::log::level::fatal, "runtime.terminate", "std::terminate called");
  const sl::assert_report report{.expression = "std::terminate",
                                 .message = "std::terminate called",
                                 .where = std::source_location::current(),
                                 .stacktrace = {}};
  sl::default_assert_handler(report);
}

} // namespace

void install_terminate_report() noexcept { std::set_terminate(&terminate_report); }

} // namespace sl::runtime
