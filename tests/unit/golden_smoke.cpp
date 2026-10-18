#include "sl_golden.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include <sl/core/version.hpp>

namespace {

// The sandbox banner (examples/sandbox/main.cpp) for the given build information.
std::string render_banner(const sl::core::BuildInfo &info) {
  return std::format("sl_sandbox\n"
                     "  compiler   {} {}\n"
                     "  standard   C++{}\n"
                     "  build type {}\n"
                     "  sanitizer  {}\n"
                     "  preset     {}\n"
                     "  linker     {}\n"
                     "  revision   {}\n",
                     info.compiler, info.compiler_version, info.cxx_standard, info.build_type,
                     info.sanitizer, info.preset, info.linker, info.revision);
}

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Replaces YYYY-MM-DD with <date> and absolute paths (a '/' at a word start, up to the next space)
// with <path>.
std::string mask_dates_and_paths(std::string_view line) {
  std::string out;
  std::size_t i = 0;
  while (i < line.size()) {
    const bool word_start = i == 0 || line[i - 1] == ' ';
    const bool date = i + 10 <= line.size() && is_digit(line[i]) && is_digit(line[i + 1]) &&
                      is_digit(line[i + 2]) && is_digit(line[i + 3]) && line[i + 4] == '-' &&
                      is_digit(line[i + 5]) && is_digit(line[i + 6]) && line[i + 7] == '-' &&
                      is_digit(line[i + 8]) && is_digit(line[i + 9]);
    if (date) {
      out += "<date>";
      i += 10;
    } else if (word_start && line[i] == '/') {
      out += "<path>";
      while (i < line.size() && line[i] != ' ') {
        ++i;
      }
    } else {
      out += line[i++];
    }
  }
  return out;
}

// Masks the volatile parts of a banner so the golden is the same on every compiler and preset: the
// value of each labelled field becomes a placeholder (an empty value becomes <unset>, which keeps
// "field missing" visible), and dates and paths anywhere are scrubbed. Lines without a known label
// pass through unchanged.
std::string mask_banner(std::string_view banner) {
  struct Field {
    std::string_view label;
    std::string_view placeholder;
  };
  constexpr std::array fields{
      Field{.label = "  compiler   ", .placeholder = "<compiler> <version>"},
      Field{.label = "  standard   ", .placeholder = "C++<standard>"},
      Field{.label = "  build type ", .placeholder = "<build-type>"},
      Field{.label = "  sanitizer  ", .placeholder = "<sanitizer>"},
      Field{.label = "  preset     ", .placeholder = "<preset>"},
      Field{.label = "  linker     ", .placeholder = "<linker>"},
      Field{.label = "  revision   ", .placeholder = "<revision>"},
  };
  std::string out;
  std::size_t start = 0;
  while (start < banner.size()) {
    std::size_t end = banner.find('\n', start);
    if (end == std::string_view::npos) {
      end = banner.size();
    }
    const std::string_view line = banner.substr(start, end - start);
    std::string masked{line};
    for (const Field &field : fields) {
      if (line.starts_with(field.label)) {
        const std::string_view value = line.substr(field.label.size());
        const bool unset = value.empty() || value == " " || value == "C++";
        masked = std::string{field.label} + std::string{unset ? "<unset>" : field.placeholder};
        break;
      }
    }
    out += mask_dates_and_paths(masked);
    out += '\n';
    start = end + 1;
  }
  return out;
}

} // namespace

TEST_CASE("golden: the sandbox banner for sl::core::build_info() with volatile fields masked") {
  SL_GOLDEN_CHECK("build_info", mask_banner(render_banner(sl::core::build_info())));
}

TEST_CASE("golden: masking hides versions, revisions, dates, paths and build types") {
  const sl::core::BuildInfo info{
      .compiler = "gcc",
      .compiler_version = "16.2.1",
      .cxx_standard = "26",
      .build_type = "RelWithDebInfo",
      .sanitizer = "address;undefined",
      .preset = "ci-clang",
      .linker = "mold",
      .revision = "3f2a9c1-dirty",
  };
  const std::string banner = render_banner(info);
  CHECK(banner.find("16.2.1") != std::string::npos);
  const std::string masked = mask_banner(banner);
  CHECK(masked.find("16.2.1") == std::string::npos);
  CHECK(masked.find("3f2a9c1") == std::string::npos);
  CHECK(masked.find("RelWithDebInfo") == std::string::npos);
  CHECK(masked.find("gcc") == std::string::npos);
  CHECK(mask_dates_and_paths("built 2026-10-17 in /home/x/build") == "built <date> in <path>");
}

TEST_CASE("golden: an empty build field stays visible as <unset>") {
  const sl::core::BuildInfo info{};
  CHECK(mask_banner(render_banner(info)).find("<unset>") != std::string::npos);
}

TEST_CASE("golden: line_diff names the changed line and keeps context") {
  const std::string diff = sl::golden::line_diff("a\nb\nc\n", "a\nB\nc\n", "x");
  CHECK(diff.find("--- golden/x.golden") != std::string::npos);
  CHECK(diff.find("@@ -1 +1 @@") != std::string::npos);
  CHECK(diff.find("\n-b\n") != std::string::npos);
  CHECK(diff.find("\n+B\n") != std::string::npos);
  CHECK(sl::golden::line_diff("same\n", "same\n", "x").empty());
}
