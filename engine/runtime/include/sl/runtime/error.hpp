#pragma once

// The runtime error domain (WO-076, SL-ARCH-003 ARCH3-R017/R018 and `error_codes`, ADR-0022,
// ADR-0009). Runtime codes are sl::errc values in 6000-6999 of error_domain::runtime, declared once
// in sl/core/error.hpp; this header names them as a table and gives the runtime its make_error.
// Layer L0: includes no other runtime header. Pure; any thread.

#include "sl/core/error.hpp"

#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

namespace sl::runtime {

/// One row of SL-ARCH-003 `error_codes`: the errc name, its number and its area.
struct error_code_info {
  std::string_view name;
  std::uint32_t number = 0;
  std::string_view area;
};

/// Every runtime code, in the order of SL-ARCH-003 `error_codes` (the contract dump reads it).
[[nodiscard]] std::span<const error_code_info> error_code_table() noexcept;

/// The `table_version` of SL-ARCH-003 `error_codes` (an input of runtime_abi_digest, ARCH3-R032).
inline constexpr std::uint32_t error_code_table_version = 1;

/// True when `code` is an enumerator of the runtime domain (6000-6999 and named); reserved numbers
/// are not.
[[nodiscard]] constexpr bool is_runtime_code(errc code) noexcept {
  return domain(code) == error_domain::runtime && to_string(code) != "unknown";
}

/// `return make_error(errc::invalid_state, "start after shutdown");` from a function returning
/// result<T>. The const char * overload keeps an unqualified call inside sl::runtime unambiguous
/// against sl::make_error (ADL on errc).
[[nodiscard]] inline std::unexpected<error>
make_error(errc code, std::string_view message,
           std::source_location where = std::source_location::current()) {
  return sl::make_error(code, std::string(message), where);
}

[[nodiscard]] inline std::unexpected<error>
make_error(errc code, const char *message,
           std::source_location where = std::source_location::current()) {
  return sl::make_error(code, std::string(message), where);
}

} // namespace sl::runtime
