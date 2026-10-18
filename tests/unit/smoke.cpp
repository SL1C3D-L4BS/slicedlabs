#include <doctest/doctest.h>

#include <sl/core/version.hpp>

TEST_CASE("sl::core::build_info is baked in at configure time") {
  const sl::core::BuildInfo info = sl::core::build_info();
  CHECK(!info.compiler.empty());
  CHECK(!info.cxx_standard.empty());
  CHECK(!info.preset.empty());
}
