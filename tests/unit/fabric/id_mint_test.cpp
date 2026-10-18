// FAB-INV-ID-02 (SL-ARCH-001 ARCH1-R02): the deterministic_source mint sequence equals the golden
// list (computed independently with Python hashlib from the SL-ARCH-001 formulas);
// os_entropy_source::create passes its self-test; 1e5 random draws are distinct.

#include "sl/fabric/id.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::fabric::deterministic_source;
using sl::fabric::id_class;
using sl::fabric::mint_scheme;
using sl::fabric::record_id;

std::string hex(std::span<const std::uint8_t> b) {
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t v : b) {
    out.push_back(digits[v >> 4U]);
    out.push_back(digits[v & 0x0FU]);
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-ID-02: deterministic_source blocks follow the slf-drbg/1 formula") {
  deterministic_source src{42, "test"};
  std::array<std::uint8_t, 64> out{};
  REQUIRE(src.fill(out).has_value());
  const std::string block0 = hex(std::span{out}.first(32));
  const std::string block1 = hex(std::span{out}.subspan(32));
  CHECK(block0 == "63f0910910bbaf7ee505ff8acc932caea53b339a30384ea29a6d811ddd85e350");
  CHECK(block1 == "3e16c8e24b3fe65965f158f7cd1e63f2e9451c6154fa7c7fdf98b2d0d26bab14");

  // The stream is the same however it is cut into fills.
  deterministic_source pieces{42, "test"};
  std::array<std::uint8_t, 64> again{};
  for (std::size_t at = 0, step = 1; at < again.size(); at += step, ++step) {
    const std::size_t n = std::min(step, again.size() - at);
    REQUIRE(pieces.fill(std::span{again}.subspan(at, n)).has_value());
  }
  CHECK(again == out);
}

TEST_CASE("FAB-INV-ID-02: deterministic mint sequences equal the golden lists") {
  deterministic_source assets{42, "test"};
  for (const std::string_view want :
       {"ast_0405HZ148922XTYZQ50QZRNK4K", "ast_0404PAX99V6ED30E2EMAD6V08X",
        "ast_0407ERBRTG7RBCHRJB7ZK5JSFH", "ast_0405CFFK8YCFSEJH8WC5AFMZ3Z",
        "ast_0407FSHCPGT9NTP50KBYA3YB06"}) {
    const auto id = sl::fabric::mint(id_class::asset, assets);
    REQUIRE(id.has_value());
    CHECK(id->to_string() == want);
    CHECK(id->cls() == id_class::asset);
    CHECK(id->scheme() == mint_scheme::random);
  }
  deterministic_source entities{7, "replay/entity"};
  for (const std::string_view want :
       {"ent_05077BCXNW5ZEE6TMKZM4B4VVJ", "ent_0504FA304381EFAB73SFVE3HNY",
        "ent_0505BKAHJEAQSVH1HG6NRH21KP"}) {
    const auto id = sl::fabric::mint(id_class::entity, entities);
    REQUIRE(id.has_value());
    CHECK(id->to_string() == want);
  }
  // Seed and stream both separate sequences.
  deterministic_source other_seed{43, "test"};
  deterministic_source other_stream{42, "tesu"};
  const auto a = sl::fabric::mint(id_class::asset, other_seed);
  const auto b = sl::fabric::mint(id_class::asset, other_stream);
  CHECK(a->to_string() != "ast_0405HZ148922XTYZQ50QZRNK4K");
  CHECK(b->to_string() != "ast_0405HZ148922XTYZQ50QZRNK4K");
  CHECK(*a != *b);
}

TEST_CASE("FAB-INV-ID-02: os_entropy_source::create passes its self-test") {
  auto src = sl::fabric::os_entropy_source::create();
  REQUIRE(src.has_value());
  CHECK(sl::fabric::entropy_self_test(*src).has_value());
  const auto id = sl::fabric::mint(id_class::tx, *src);
  REQUIRE(id.has_value());
  CHECK(id->cls() == id_class::tx);
  CHECK(id->scheme() == mint_scheme::random);
}

TEST_CASE("FAB-INV-ID-02: 1e5 random draws are distinct") {
  auto src = sl::fabric::os_entropy_source::create();
  REQUIRE(src.has_value());
  std::vector<record_id> ids;
  ids.reserve(100'000);
  for (int i = 0; i < 100'000; ++i) {
    const auto id = sl::fabric::mint(id_class::entity, *src);
    REQUIRE(id.has_value());
    ids.push_back(*id);
  }
  std::ranges::sort(ids);
  CHECK(std::ranges::adjacent_find(ids) == ids.end());
}
