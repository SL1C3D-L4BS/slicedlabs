#include <filesystem>
#include <system_error>

bool present(const std::filesystem::path &p) {
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) {
    return true;
  }
  return std::filesystem::exists(p); // PLANT
}
