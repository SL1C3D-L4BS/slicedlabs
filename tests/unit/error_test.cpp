#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <sl/core/error.hpp>

namespace {

sl::result<int> parse_digit(char c) {
  if (c < '0' || c > '9') {
    return sl::make_error(sl::errc::invalid_argument, std::string("not a digit: ") + c);
  }
  return c - '0';
}

sl::result<void> require_even(int v) {
  if (v % 2 != 0) {
    return sl::make_error(sl::errc::out_of_range, "odd");
  }
  return {};
}

} // namespace

static_assert(sl::domain(sl::errc::ok) == sl::error_domain::none);
static_assert(sl::domain(sl::errc::internal) == sl::error_domain::core);
static_assert(sl::to_string(sl::errc::device_lost) == "device_lost");

TEST_CASE("error codes map to their domain by numeric range") {
  using sl::errc;
  using sl::error_domain;
  CHECK(sl::domain(errc::ok) == error_domain::none);
  for (const errc c : {errc::invalid_argument, errc::out_of_range, errc::not_found,
                       errc::already_exists, errc::unsupported, errc::internal}) {
    CHECK(sl::domain(c) == error_domain::core);
    CHECK(static_cast<unsigned>(c) >= 1000);
    CHECK(static_cast<unsigned>(c) <= 1999);
  }
  for (const errc c :
       {errc::platform_failure, errc::io_error, errc::permission_denied, errc::timed_out}) {
    CHECK(sl::domain(c) == error_domain::platform);
  }
  for (const errc c : {errc::graphics_failure, errc::device_lost, errc::out_of_device_memory,
                       errc::unsupported_format}) {
    CHECK(sl::domain(c) == error_domain::graphics);
  }
  for (const errc c : {errc::asset_failure, errc::asset_not_found, errc::asset_corrupt,
                       errc::asset_version_mismatch}) {
    CHECK(sl::domain(c) == error_domain::asset);
  }
  CHECK(sl::domain(static_cast<errc>(9000)) == error_domain::unknown);
  CHECK(sl::to_string(error_domain::graphics) == "graphics");
  CHECK(sl::to_string(error_domain::none) == "none");
}

TEST_CASE("every error code has a name; unknown values say so") {
  using sl::errc;
  CHECK(sl::to_string(errc::ok) == "ok");
  CHECK(sl::to_string(errc::invalid_argument) == "invalid_argument");
  CHECK(sl::to_string(errc::not_found) == "not_found");
  CHECK(sl::to_string(errc::io_error) == "io_error");
  CHECK(sl::to_string(errc::asset_version_mismatch) == "asset_version_mismatch");
  for (const errc c :
       {errc::out_of_range, errc::already_exists, errc::unsupported, errc::internal,
        errc::platform_failure, errc::permission_denied, errc::timed_out, errc::graphics_failure,
        errc::device_lost, errc::out_of_device_memory, errc::unsupported_format,
        errc::asset_failure, errc::asset_not_found, errc::asset_corrupt}) {
    CHECK(sl::to_string(c) != "unknown");
  }
  CHECK(sl::to_string(static_cast<errc>(1999)) == "unknown");
}

TEST_CASE("result<T> carries a value or an error with its source location") {
  const sl::result<int> ok = parse_digit('7');
  REQUIRE(ok.has_value());
  CHECK(*ok == 7);

  const sl::result<int> bad = parse_digit('x');
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::invalid_argument);
  CHECK(bad.error().message == "not a digit: x");
  CHECK(std::string_view{bad.error().where.file_name()}.ends_with("error_test.cpp"));
  CHECK(bad.error().where.line() > 0);

  const auto doubled = parse_digit('4').transform([](int v) { return v * 2; });
  CHECK(doubled.value_or(-1) == 8);
  const auto chained =
      parse_digit('3').and_then([](int v) { return parse_digit(v == 3 ? 'q' : '1'); });
  CHECK(chained.error().code == sl::errc::invalid_argument);
  const auto recovered =
      parse_digit('?').or_else([](const sl::error &) -> sl::result<int> { return 0; });
  CHECK(recovered.value() == 0);
}

TEST_CASE("result<void> and describe()") {
  CHECK(require_even(4).has_value());
  const sl::result<void> odd = require_even(3);
  REQUIRE_FALSE(odd.has_value());
  const std::string text = sl::describe(odd.error());
  CHECK(text.starts_with("core.out_of_range (1001): odd [error_test.cpp:"));
  CHECK(text.ends_with(']'));
}
