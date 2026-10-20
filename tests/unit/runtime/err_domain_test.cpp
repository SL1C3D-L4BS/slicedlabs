// RT-INV-ERR-02 (SL-ARCH-003 ARCH3-R017, ADR-0022): domain() of every runtime code is
// error_domain::runtime, to_string(error_domain::runtime) is "runtime" and a code in 7000-7999
// stays unknown. error_domain::runtime is appended after unknown, so no existing domain value
// moves.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

namespace {

using sl::errc;
using sl::error_domain;

} // namespace

static_assert(sl::domain(errc::runtime_failure) == error_domain::runtime);
static_assert(sl::domain(errc::projection_stale) == error_domain::runtime);

TEST_CASE("RT-INV-ERR-02: every code of the error_codes table is in error_domain::runtime") {
  for (const sl::runtime::error_code_info &row : sl::runtime::error_code_table()) {
    INFO(std::string(row.name));
    CHECK(sl::domain(static_cast<errc>(row.number)) == error_domain::runtime);
  }
  for (std::uint32_t n = 6000; n <= 6999; ++n) {
    INFO("number " << n);
    CHECK(sl::domain(static_cast<errc>(n)) == error_domain::runtime);
  }
}

TEST_CASE("RT-INV-ERR-02: error_domain::runtime follows unknown and is named runtime") {
  CHECK(sl::to_string(error_domain::runtime) == "runtime");
  CHECK(static_cast<unsigned>(error_domain::unknown) == 6);
  CHECK(static_cast<unsigned>(error_domain::runtime) == 7);
  CHECK(static_cast<unsigned>(error_domain::fabric) == 5);
  CHECK(sl::to_string(error_domain::unknown) == "unknown");
  CHECK(sl::to_string(error_domain::fabric) == "fabric");
}

TEST_CASE("RT-INV-ERR-02: a code in 7000-7999 stays unknown") {
  for (std::uint32_t n = 7000; n <= 7999; ++n) {
    INFO("number " << n);
    CHECK(sl::domain(static_cast<errc>(n)) == error_domain::unknown);
    CHECK(sl::to_string(static_cast<errc>(n)) == "unknown");
    CHECK_FALSE(sl::runtime::is_runtime_code(static_cast<errc>(n)));
  }
  CHECK(sl::domain(errc::event_invalid) == error_domain::fabric);
  CHECK(sl::domain(static_cast<errc>(5999)) == error_domain::fabric);
}
