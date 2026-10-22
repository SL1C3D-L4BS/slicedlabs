// The stress harness (WO-098, SL-ARCH-003 ARCH3-R162 to R165): scale, seeds, the rerun line, the
// file cap and the watchdog. See stress_harness.hpp.

#include "stress_harness.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace sl::runtime::testing {

namespace {

constexpr double scale_min = 0.01;
constexpr double scale_max = 10.0;

// The decimal value of environment variable `name`, or nothing when unset or not a number.
bool env_u64(const char *name, std::uint64_t &out) noexcept {
  const char *text = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

} // namespace

double stress_scale() noexcept {
  const char *text =
      std::getenv("SL_RUNTIME_STRESS_SCALE"); // NOLINT(concurrency-mt-unsafe): read only
  if (text == nullptr || *text == '\0') {
    return 1.0;
  }
  char *end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || !std::isfinite(value)) {
    return 1.0;
  }
  return std::clamp(value, scale_min, scale_max);
}

std::uint64_t scaled(std::uint64_t n) noexcept {
  const double v = static_cast<double>(n) * stress_scale();
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(v));
}

std::uint64_t stress_seed(std::uint64_t fixed) noexcept {
  std::uint64_t seed = 0;
  return env_u64("SL_RUNTIME_SOAK_SEED", seed) ? seed : fixed;
}

bool soak_seed_set() noexcept {
  std::uint64_t seed = 0;
  return env_u64("SL_RUNTIME_SOAK_SEED", seed);
}

seed_guard::seed_guard(std::string_view test, std::uint64_t seed)
    : seed_(seed), line_("seed " + std::to_string(seed) + "; rerun: SL_RUNTIME_SOAK_SEED=" +
                         std::to_string(seed) + " ctest --preset debug -R " + std::string(test)) {}

cap_timer::cap_timer(double cap_s) noexcept : start_(clock_.now()), cap_s_(cap_s) {}

double cap_timer::elapsed_s() const noexcept {
  return std::chrono::duration<double>(clock_.now() - start_).count();
}

result<void> run_watched(const std::function<void()> &body, std::chrono::milliseconds limit) {
  std::mutex mutex;
  std::condition_variable cv;
  bool finished = false;
  os_thread_factory factory(1);
  auto handle = factory.spawn("stress", [&] {
    body();
    {
      const std::scoped_lock lock(mutex);
      finished = true;
    }
    cv.notify_all();
  });
  if (!handle) {
    return std::unexpected(std::move(handle.error()));
  }
  {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, limit, [&] { return finished; })) {
      std::println(stderr, "stress watchdog: no completion within {} ms", limit.count());
      std::fflush(stderr);
      std::abort();
    }
  }
  handle->join();
  return {};
}

} // namespace sl::runtime::testing
