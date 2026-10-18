// FAB-INV-ID-01 (SL-ARCH-001 ARCH1-R01): record id bytes and text round trip; parse rejects a bad
// prefix, a prefix/class mismatch, a wrong length, characters outside the alphabet and a first
// character above 7.

#include "sl/fabric/id.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using sl::fabric::id_class;
using sl::fabric::record_id;

void check_parse_fails(std::string_view text, std::string_view why) {
  CAPTURE(text);
  const auto r = record_id::parse(text);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::id_parse);
  CHECK(r.error().message.find(why) != std::string::npos);
}

} // namespace

TEST_CASE("FAB-INV-ID-01: the 15 classes, their names and prefixes") {
  const auto table =
      std::to_array<std::pair<std::string_view, std::string_view>>({{"project", "prj"},
                                                                    {"schema", "sch"},
                                                                    {"type", "typ"},
                                                                    {"asset", "ast"},
                                                                    {"entity", "ent"},
                                                                    {"instance", "ins"},
                                                                    {"derived", "drv"},
                                                                    {"tx", "txn"},
                                                                    {"event", "evt"},
                                                                    {"actor", "act"},
                                                                    {"package", "pkg"},
                                                                    {"element", "elm"},
                                                                    {"namespace", "nsp"},
                                                                    {"operation", "ope"},
                                                                    {"import", "imp"}});
  std::uint8_t n = 1;
  for (const auto &[name, pfx] : table) {
    const auto c = static_cast<id_class>(n++);
    CHECK(sl::fabric::is_valid(c));
    CHECK(sl::fabric::to_string(c) == name);
    CHECK(sl::fabric::prefix(c) == pfx);
    CHECK(sl::fabric::class_from_prefix(pfx) == c);
  }
  CHECK(n == 16);
  CHECK_FALSE(sl::fabric::is_valid(id_class::none));
  CHECK_FALSE(sl::fabric::is_valid(static_cast<id_class>(16)));
  CHECK(sl::fabric::prefix(id_class::none).empty());
  CHECK_FALSE(sl::fabric::class_from_prefix("xyz").has_value());
  CHECK_FALSE(sl::fabric::class_from_prefix("").has_value());
}

TEST_CASE("FAB-INV-ID-01: known ids have the expected text (independent Python encoder)") {
  const record_id ns = sl::fabric::well_known(id_class::namespace_, 1);
  CHECK(ns.to_string() == "nsp_0D0C0000000000000000000001");
  CHECK(sl::fabric::well_known(id_class::entity, 1).to_string() ==
        "ent_050C0000000000000000000001");
  std::array<std::uint8_t, 16> top{};
  top.fill(0xFF);
  top[0] = 15;
  top[1] = 4;
  const auto max = record_id::from_bytes(top);
  REQUIRE(max.has_value());
  CHECK(max->to_string() == "imp_0F0KZZZZZZZZZZZZZZZZZZZZZZ");
  CHECK(record_id{}.to_string() == "none");
  CHECK(record_id{}.is_null());
}

TEST_CASE("FAB-INV-ID-01: bytes and text round trip") {
  sl::fabric::deterministic_source src{1, "id-text"};
  for (std::uint8_t n = 1; n <= 15; ++n) {
    for (int i = 0; i < 50; ++i) {
      const auto id = sl::fabric::mint(static_cast<id_class>(n), src);
      REQUIRE(id.has_value());
      const std::string text = id->to_string();
      CAPTURE(text);
      CHECK(text.size() == record_id::text_size);
      CHECK(text[4] <= '7');
      const auto back = record_id::parse(text);
      REQUIRE(back.has_value());
      CHECK(*back == *id);
      const auto from = record_id::from_bytes(id->bytes());
      REQUIRE(from.has_value());
      CHECK(*from == *id);
    }
  }
}

TEST_CASE("FAB-INV-ID-01: parse rejects malformed text") {
  const std::string good = sl::fabric::well_known(id_class::asset, 7).to_string();
  REQUIRE(record_id::parse(good).has_value());
  check_parse_fails("", "length");
  check_parse_fails(good.substr(0, 29), "length");
  check_parse_fails(good + "0", "length");
  check_parse_fails("abc_040C0000000000000000000007", "unknown prefix");
  check_parse_fails("AST_040C0000000000000000000007", "unknown prefix");
  check_parse_fails("ast-040C0000000000000000000007", "'_'");
  // prefix/class mismatch: an asset id's digits under the entity prefix
  check_parse_fails("ent_040C0000000000000000000007", "disagrees with class byte");
  // alphabet: I, L, O, U, lower case and punctuation are not canonical digits
  check_parse_fails("ast_040C000000000000000000000I", "Crockford");
  check_parse_fails("ast_040C000000000000000000000L", "Crockford");
  check_parse_fails("ast_040C000000000000000000000O", "Crockford");
  check_parse_fails("ast_040C000000000000000000000U", "Crockford");
  check_parse_fails("ast_040c0000000000000000000007", "Crockford");
  check_parse_fails("ast_040C00000000000000000000-7", "Crockford");
  // first character above 7 would need 129 or 130 bits
  check_parse_fails("ast_840C0000000000000000000007", "first digit above 7");
  check_parse_fails("ast_Z40C0000000000000000000007", "first digit above 7");
  // scheme byte 0 and 5 are not assigned
  check_parse_fails("ast_0400000000000000000000000Z", "scheme 0");
  check_parse_fails("ast_040M0000000000000000000000", "scheme 5");
}

TEST_CASE("FAB-INV-ID-01: from_bytes rejects a wrong length, class or scheme") {
  std::array<std::uint8_t, 17> raw{};
  raw[0] = 4;
  raw[1] = 1;
  CHECK(record_id::from_bytes(sl::fabric::byte_span{raw}.first(15)).error().code ==
        sl::errc::id_parse);
  CHECK(record_id::from_bytes(raw).error().code == sl::errc::id_parse);
  CHECK(record_id::from_bytes(sl::fabric::byte_span{raw}.first(16)).has_value());
  raw[0] = 0;
  CHECK(record_id::from_bytes(sl::fabric::byte_span{raw}.first(16)).error().code ==
        sl::errc::id_parse);
  raw[0] = 16;
  CHECK(record_id::from_bytes(sl::fabric::byte_span{raw}.first(16)).error().code ==
        sl::errc::id_parse);
  raw[0] = 4;
  raw[1] = 5;
  CHECK(record_id::from_bytes(sl::fabric::byte_span{raw}.first(16)).error().code ==
        sl::errc::id_parse);
}
