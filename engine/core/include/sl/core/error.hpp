#pragma once

// Error-code taxonomy and the result type (WO-030, SL-EP-056, ADR-0009). Engine code reports
// recoverable failures as values, `sl::result<T>` (std::expected), never by throwing.
//
// Codes are grouped by domain in numeric ranges: core 1000-1999, platform 2000-2999,
// graphics 3000-3999, asset 4000-4999; ok is 0. A code keeps its number for life.

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace sl {

enum class errc : std::uint32_t {
  ok = 0,
  // core 1000-1999
  invalid_argument = 1000,
  out_of_range = 1001,
  not_found = 1002,
  already_exists = 1003,
  unsupported = 1004,
  internal = 1005,
  // platform 2000-2999
  platform_failure = 2000,
  io_error = 2001,
  permission_denied = 2002,
  timed_out = 2003,
  // graphics 3000-3999
  graphics_failure = 3000,
  device_lost = 3001,
  out_of_device_memory = 3002,
  unsupported_format = 3003,
  // asset 4000-4999
  asset_failure = 4000,
  asset_not_found = 4001,
  asset_corrupt = 4002,
  asset_version_mismatch = 4003,
};

enum class error_domain : std::uint8_t { none, core, platform, graphics, asset, unknown };

/// The domain a code belongs to, by its numeric range (ok is `none`).
[[nodiscard]] constexpr error_domain domain(errc code) noexcept {
  const auto n = static_cast<std::uint32_t>(code);
  if (n == 0) {
    return error_domain::none;
  }
  switch (n / 1000) {
  case 1:
    return error_domain::core;
  case 2:
    return error_domain::platform;
  case 3:
    return error_domain::graphics;
  case 4:
    return error_domain::asset;
  default:
    return error_domain::unknown;
  }
}

[[nodiscard]] constexpr std::string_view to_string(error_domain d) noexcept {
  switch (d) {
  case error_domain::none:
    return "none";
  case error_domain::core:
    return "core";
  case error_domain::platform:
    return "platform";
  case error_domain::graphics:
    return "graphics";
  case error_domain::asset:
    return "asset";
  case error_domain::unknown:
    return "unknown";
  }
  return "unknown";
}

/// The enumerator name ("invalid_argument"); "unknown" for a value that is not an enumerator.
[[nodiscard]] constexpr std::string_view to_string(errc code) noexcept {
  switch (code) {
  case errc::ok:
    return "ok";
  case errc::invalid_argument:
    return "invalid_argument";
  case errc::out_of_range:
    return "out_of_range";
  case errc::not_found:
    return "not_found";
  case errc::already_exists:
    return "already_exists";
  case errc::unsupported:
    return "unsupported";
  case errc::internal:
    return "internal";
  case errc::platform_failure:
    return "platform_failure";
  case errc::io_error:
    return "io_error";
  case errc::permission_denied:
    return "permission_denied";
  case errc::timed_out:
    return "timed_out";
  case errc::graphics_failure:
    return "graphics_failure";
  case errc::device_lost:
    return "device_lost";
  case errc::out_of_device_memory:
    return "out_of_device_memory";
  case errc::unsupported_format:
    return "unsupported_format";
  case errc::asset_failure:
    return "asset_failure";
  case errc::asset_not_found:
    return "asset_not_found";
  case errc::asset_corrupt:
    return "asset_corrupt";
  case errc::asset_version_mismatch:
    return "asset_version_mismatch";
  }
  return "unknown";
}

/// A failure: its code, a human message and where it was made.
struct error {
  errc code = errc::internal;
  std::string message;
  std::source_location where;
};

/// The engine's result type: a value or an sl::error (ADR-0009).
template <class T> using result = std::expected<T, error>;

/// `return sl::make_error(sl::errc::not_found, "no such mesh");` from a function returning
/// result<T>.
[[nodiscard]] inline std::unexpected<error>
make_error(errc code, std::string message,
           std::source_location where = std::source_location::current()) {
  return std::unexpected<error>(error{.code = code, .message = std::move(message), .where = where});
}

/// "core.not_found (1002): no such mesh [file:line]".
[[nodiscard]] std::string describe(const error &e);

} // namespace sl
