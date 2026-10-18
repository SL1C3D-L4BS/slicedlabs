// FAB-INV-SER-06 (SL-ARCH-001 ARCH1-R11): every byte flip and every truncation of 20 encoded
// samples decodes to an error or to a value that re-encodes to exactly the corrupted bytes (the
// decoder accepts only canonical input), with no crash and no sanitizer finding. The flip masks
// per byte scale with SL_FABRIC_SWEEP_SCALE (all 9 masks at 1.0).

#include "sl/fabric/codec.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

double sweep_scale() {
  const char *env = std::getenv("SL_FABRIC_SWEEP_SCALE"); // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr) {
    return 1.0;
  }
  const std::string_view s(env);
  double v = 1.0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc{} && v > 0.0 ? std::min(v, 1.0) : 1.0;
}

/// Twenty samples: the test-schema records and generated values of every kind.
std::vector<std::vector<std::uint8_t>> samples() {
  std::vector<std::vector<std::uint8_t>> out;
  out.push_back(must(encode(must(testing::sample_entity("hero")))));
  out.push_back(must(encode(must(testing::sample_scene("level", 3)))));
  out.push_back(must(encode(must(testing::make_vec3(1.5, -2.0, 1e300)))));
  testing::value_gen gen(606, testing::value_gen_options{.max_depth = 4, .max_items = 4});
  while (out.size() < 20) {
    auto bytes = must(encode(gen.any()));
    if (bytes.size() >= 8 && bytes.size() <= 400) {
      out.push_back(std::move(bytes));
    }
  }
  return out;
}

struct tally {
  std::size_t rejected = 0;
  std::size_t accepted = 0;
};

/// Decoding `input` either fails or yields a value whose encoding is `input` itself.
void check_one(const std::vector<std::uint8_t> &input, tally &t) {
  const auto v = decode(input);
  if (!v) {
    ++t.rejected;
    return;
  }
  ++t.accepted;
  const auto again = encode(*v);
  REQUIRE(again.has_value());
  CHECK(*again == input);
}

} // namespace

TEST_CASE("FAB-INV-SER-06: every byte flip and truncation of 20 samples is an error or canonical") {
  constexpr std::array<std::uint8_t, 9> all_masks = {0x01, 0x02, 0x04, 0x08, 0x10,
                                                     0x20, 0x40, 0x80, 0xFF};
  const auto mask_count =
      std::max<std::size_t>(1, static_cast<std::size_t>(sweep_scale() * all_masks.size()));
  const auto all = samples();
  REQUIRE(all.size() == 20);
  tally t;
  std::size_t mutations = 0;
  for (std::size_t s = 0; s < all.size(); ++s) {
    CAPTURE(s);
    const auto &original = all[s];
    tally clean;
    check_one(original, clean);
    CHECK(clean.accepted == 1);
    for (std::size_t len = 0; len < original.size(); ++len) {
      const std::vector<std::uint8_t> prefix(original.begin(),
                                             original.begin() + static_cast<std::ptrdiff_t>(len));
      check_one(prefix, t);
      ++mutations;
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
      // Rotate which masks run at a reduced scale so every mask is still exercised.
      for (std::size_t m = 0; m < mask_count; ++m) {
        std::vector<std::uint8_t> flipped = original;
        flipped[i] ^= std::span(all_masks)[(i + m) % all_masks.size()];
        check_one(flipped, t);
        ++mutations;
      }
    }
    // A trailing byte is never canonical.
    std::vector<std::uint8_t> longer = original;
    longer.push_back(0x01);
    const auto r = decode(longer);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.starts_with("R-SER-TRAILING"));
  }
  CHECK(t.rejected + t.accepted == mutations);
  CHECK(t.rejected > 0);
  MESSAGE("mutations " << mutations << ": rejected " << t.rejected << ", canonical " << t.accepted);
}
