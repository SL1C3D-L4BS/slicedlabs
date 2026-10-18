#include "sl/core/error.hpp"

#include <format>

namespace sl {

std::string describe(const error &e) {
  std::string_view file = e.where.file_name();
  if (const std::size_t slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
    file = file.substr(slash + 1);
  }
  return std::format("{}.{} ({}): {} [{}:{}]", to_string(domain(e.code)), to_string(e.code),
                     static_cast<std::uint32_t>(e.code), e.message, file, e.where.line());
}

} // namespace sl
