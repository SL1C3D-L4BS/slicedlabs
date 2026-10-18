#include <doctest/doctest.h>

#include <string_view>

#include <sl/core/version.hpp>

TEST_CASE("sl::core::build_info reports the configured C++ standard") {
  CHECK(sl::core::build_info().cxx_standard == std::string_view{SL_EXPECTED_CXX_STANDARD});
}

TEST_CASE("sl::core::build_info is stable across calls") {
  const sl::core::BuildInfo a = sl::core::build_info();
  const sl::core::BuildInfo b = sl::core::build_info();
  CHECK(a.compiler == b.compiler);
  CHECK(a.revision == b.revision);
}
