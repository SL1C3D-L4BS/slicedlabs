// The checks sl_sandbox --self-check makes (examples/sandbox/main.cpp), run in-process against
// sl::core.
#include <doctest/doctest.h>

#include <initializer_list>
#include <string_view>

#include <sl/core/version.hpp>

TEST_CASE("every build_info field the sandbox self-check requires is provided") {
  const sl::core::BuildInfo info = sl::core::build_info();
  const std::initializer_list<std::string_view> fields = {
      info.compiler,  info.compiler_version, info.cxx_standard, info.build_type,
      info.sanitizer, info.preset,           info.linker,       info.revision};
  for (const std::string_view field : fields) {
    CHECK_FALSE(field.empty());
  }
}

TEST_CASE("the sandbox and this test are built with the same toolchain settings") {
  const sl::core::BuildInfo info = sl::core::build_info();
  CHECK((info.cxx_standard == "23" || info.cxx_standard == "26"));
}
