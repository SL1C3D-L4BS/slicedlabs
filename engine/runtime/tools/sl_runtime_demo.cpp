// sl_runtime_demo: the gate 100 headless runner (WO-099, SL-ARCH-003 ARCH3-R167 and R168). Runs
// the lantern-field scene (demo/scene.hpp) and writes the gate evidence under --evidence-dir.

#include "demo/scene.hpp"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
  std::vector<std::string_view> args;
  const std::span<char *> all(argv, static_cast<std::size_t>(argc));
  for (std::size_t i = 1; i < all.size(); ++i) {
    args.emplace_back(all[i]);
  }
  const auto write = [](std::FILE *stream) {
    return [stream](std::string_view text) {
      std::fwrite(text.data(), 1, text.size(), stream);
      std::fflush(stream);
    };
  };
  return sl::runtime::demo::run_cli(args, write(stdout), write(stderr));
}
