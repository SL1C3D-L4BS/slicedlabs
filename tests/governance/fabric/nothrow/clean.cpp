// A throw in a comment, try { } catch, v.at(0), std::stoi, std::unordered_map, new T and delete p
// are prose.
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

struct no_copy {
  no_copy(const no_copy &) = delete;
  no_copy &operator=(const no_copy &) = delete;
  static void *operator new(unsigned long) noexcept;
  static void operator delete(void *) noexcept;
};

const char *text = "throw try catch .at( std::stoi std::unordered_set new delete";
const char quote = '"';
const int million = 1'000'000;

bool probe(const fs::path &p) {
  std::error_code ec;
  const bool a = std::filesystem::exists(p, ec);
  const bool b = fs::is_directory(p, ec);
  std::filesystem::directory_iterator it(p, ec);
  return a && b && !ec && it == std::filesystem::directory_iterator();
}

std::map<std::string, int> ordered;
