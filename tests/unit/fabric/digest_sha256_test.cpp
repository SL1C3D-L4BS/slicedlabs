// FAB-INV-DG-01 (SL-ARCH-001 ARCH1-R09): the in-house SHA-256 matches the FIPS 180-4 / NIST
// vectors, including one million 'a', and incremental hashing equals one-shot hashing.

#include "sl/fabric/digest.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string hex(const sl::fabric::sha256::value_type &d) {
  return sl::fabric::digest{.algo = sl::fabric::hash_algo::sha256, .bytes = d}.to_hex();
}

std::vector<std::uint8_t> pattern(std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>(i % 251);
  }
  return v;
}

} // namespace

TEST_CASE("FAB-INV-DG-01: SHA-256 NIST vectors") {
  using sl::fabric::sha256;
  CHECK(hex(sha256::hash(std::string_view{})) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256::hash(std::string_view{"abc"})) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(sha256::hash(
            std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex(sha256::hash(std::string_view{
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnop"
            "qrlmnopqrsmnopqrstnopqrstu"})) ==
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("FAB-INV-DG-01: SHA-256 of one million 'a'") {
  using sl::fabric::sha256;
  const std::string million(1'000'000, 'a');
  CHECK(hex(sha256::hash(million)) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // The same million bytes fed in uneven pieces.
  sha256 h;
  std::size_t fed = 0;
  std::size_t piece = 1;
  while (fed < million.size()) {
    const std::size_t n = std::min(piece, million.size() - fed);
    h.update(std::string_view{million}.substr(fed, n));
    fed += n;
    piece = (((piece * 7) + 3) % 1000) + 1;
  }
  CHECK(hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("FAB-INV-DG-01: SHA-256 padding boundaries (hashlib values)") {
  using sl::fabric::sha256;
  struct row {
    std::size_t size = 0;
    std::string_view hex;
  };
  const auto rows = std::to_array<row>({
      {.size = 55, .hex = "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
      {.size = 56, .hex = "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
      {.size = 63, .hex = "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
      {.size = 64, .hex = "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
      {.size = 65, .hex = "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
      {.size = 119, .hex = "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
      {.size = 120, .hex = "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
      {.size = 128, .hex = "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
  });
  for (const row &r : rows) {
    CAPTURE(r.size);
    CHECK(hex(sha256::hash(pattern(r.size))) == r.hex);
  }
}

TEST_CASE("FAB-INV-DG-01: incremental SHA-256 equals one-shot at every split point") {
  using sl::fabric::byte_span;
  using sl::fabric::sha256;
  const std::vector<std::uint8_t> msg = pattern(200);
  for (std::size_t n = 0; n <= msg.size(); ++n) {
    const byte_span all = byte_span{msg}.first(n);
    const sha256::value_type expected = sha256::hash(all);
    for (std::size_t split = 0; split <= n; ++split) {
      sha256 h;
      h.update(all.first(split));
      h.update(all.subspan(split));
      if (h.finish() != expected) {
        FAIL("split " << split << " of " << n << " differs");
      }
    }
  }
  // finish() leaves the state usable: absorbing more continues the same message.
  sha256 h;
  h.update(byte_span{msg}.first(70));
  const auto prefix_digest = h.finish();
  CHECK(prefix_digest == sha256::hash(byte_span{msg}.first(70)));
  h.update(byte_span{msg}.subspan(70));
  CHECK(h.finish() == sha256::hash(msg));
  h.reset();
  CHECK(hex(h.finish()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
