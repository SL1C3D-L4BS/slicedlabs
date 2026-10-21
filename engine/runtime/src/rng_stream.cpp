// rng_stream (WO-084, SL-ARCH-003 ARCH3-R089, R087, ADR-0026): std::mt19937_64 seeded from
// SHA-256("slr-rng/1" || seed u64le || stream u64le)[0:8] read little-endian, with the
// engine-defined mappings next_below (rejection of the biased tail) and next_unit (53 bits); no
// std distribution. The state text is the engine's operator<< under the classic locale.

#include "sl/core/assert.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <locale>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace sl::runtime::det {

namespace {

constexpr std::string_view rng_domain = "slr-rng/1";

std::uint64_t seed_value(std::uint64_t seed, std::uint64_t stream) noexcept {
  std::array<std::uint8_t, 16> storage{};
  const std::span<std::uint8_t> words(storage);
  for (unsigned i = 0; i < 8U; ++i) {
    words[i] = static_cast<std::uint8_t>((seed >> (8U * i)) & 0xFFU);
    words[8U + i] = static_cast<std::uint8_t>((stream >> (8U * i)) & 0xFFU);
  }
  fabric::sha256 sha;
  sha.update(rng_domain);
  sha.update(fabric::byte_span(words));
  const fabric::sha256::value_type digest = sha.finish();
  const std::span<const std::uint8_t> d(digest);
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8U; ++i) {
    v |= std::uint64_t{d[i]} << (8U * i);
  }
  return v;
}

} // namespace

rng_stream::rng_stream(std::uint64_t seed, std::uint64_t stream) noexcept
    : engine_(seed_value(seed, stream)) {}

std::uint64_t rng_stream::next_below(std::uint64_t n) noexcept {
  SL_DEBUG_ASSERT(n > 0, "rng_stream::next_below: n must be above 0");
  if (n == 0) {
    return 0;
  }
  const std::uint64_t threshold = (std::uint64_t{0} - n) % n;
  for (;;) {
    const std::uint64_t x = engine_();
    if (x >= threshold) {
      return x % n;
    }
  }
}

double rng_stream::next_unit() noexcept {
  return static_cast<double>(engine_() >> 11U) * 0x1.0p-53;
}

std::string rng_stream::capture() const {
  std::ostringstream os;
  os.imbue(std::locale::classic());
  os << engine_;
  return os.str();
}

result<void> rng_stream::restore(std::string_view text) {
  std::istringstream is{std::string(text)};
  is.imbue(std::locale::classic());
  std::mt19937_64 parsed = engine_; // overwritten by the parse; never default-seeded
  is >> parsed;
  if (is.fail()) {
    return make_error(errc::snapshot_corrupt, "rng_stream::restore: malformed engine state");
  }
  is >> std::ws;
  if (!is.eof()) {
    return make_error(errc::snapshot_corrupt, "rng_stream::restore: trailing text after the state");
  }
  engine_ = parsed;
  return {};
}

} // namespace sl::runtime::det
