// FAB-INV-ID-06 (SL-ARCH-001 ARCH1-R09): the C++ SHA-256, derived, well-known and imported ids, id
// text, the deterministic source, trie keys and object ids equal the Python hashlib vectors of
// tests/fixtures/fabric/hash/vectors.json, read from the file.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"

#include <doctest/doctest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

const json_value &vectors() {
  static const json_value doc = [] {
    const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/hash/vectors.json";
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    auto r = parse_json(s.str());
    return r ? std::move(*r) : json_value{};
  }();
  return doc;
}

const std::vector<json_value> &section(std::string_view name) {
  const json_value *s = vectors().find(name);
  REQUIRE_MESSAGE(s != nullptr, name);
  REQUIRE(!s->items.empty());
  return s->items;
}

std::string text(const json_value &v, std::string_view key) {
  const json_value *x = v.find(key);
  REQUIRE_MESSAGE(x != nullptr, key);
  return x->text;
}

std::uint64_t number(const json_value &v, std::string_view key) {
  const std::string s = text(v, key);
  std::uint64_t n = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  REQUIRE((ec == std::errc{} && end == s.data() + s.size()));
  return n;
}

std::vector<std::uint8_t> hex(const json_value &v, std::string_view key) {
  return must(from_hex(text(v, key)));
}

std::string as_chars(const std::vector<std::uint8_t> &b) { return {b.begin(), b.end()}; }

record_id id_from(const json_value &v, std::string_view key) {
  return must(record_id::from_bytes(hex(v, key)));
}

id_class class_of(const json_value &v) { return static_cast<id_class>(number(v, "class")); }

} // namespace

TEST_CASE("FAB-INV-ID-06: SHA-256 equals the hashlib vectors") {
  for (const json_value &v : section("sha256")) {
    CAPTURE(text(v, "name"));
    const json_value &input = *v.find("input");
    sha256 h;
    if (const json_value *x = input.find("hex"); x != nullptr) {
      h.update(must(from_hex(x->text)));
    } else if (const json_value *a = input.find("ascii"); a != nullptr) {
      h.update(std::string_view(a->text));
    } else {
      const json_value &rep = *input.find("repeat");
      const auto chunk = hex(rep, "hex");
      const std::uint64_t count = number(rep, "count");
      for (std::uint64_t i = 0; i < count; ++i) {
        h.update(chunk);
      }
    }
    CHECK(to_hex(h.finish()) == text(v, "sha256"));
  }
}

TEST_CASE("FAB-INV-ID-06: derived, well-known and imported ids equal the hashlib vectors") {
  for (const json_value &v : section("derive")) {
    CAPTURE(text(v, "name"));
    const record_id id = derive(class_of(v), id_from(v, "parent"), as_chars(hex(v, "key_hex")));
    CHECK(to_hex(id.bytes()) == text(v, "id"));
    CHECK(id.to_string() == text(v, "text"));
  }
  for (const json_value &v : section("well_known")) {
    CAPTURE(text(v, "name"));
    const record_id id = well_known(class_of(v), static_cast<std::uint32_t>(number(v, "ordinal")));
    CHECK(to_hex(id.bytes()) == text(v, "id"));
    CHECK(id.to_string() == text(v, "text"));
  }
  for (const json_value &v : section("imported")) {
    CAPTURE(text(v, "name"));
    const record_id id =
        imported(class_of(v), as_chars(hex(v, "system_hex")), as_chars(hex(v, "external_hex")));
    CHECK(to_hex(id.bytes()) == text(v, "id"));
    CHECK(id.to_string() == text(v, "text"));
  }
}

TEST_CASE("FAB-INV-ID-06: id text, deterministic source and trie keys equal the vectors") {
  for (const json_value &v : section("id_text")) {
    CAPTURE(text(v, "name"));
    const record_id id = id_from(v, "hex");
    CHECK(id.to_string() == text(v, "text"));
    CHECK(must(record_id::parse(text(v, "text"))) == id);
  }
  for (const json_value &v : section("id_text_reject")) {
    CAPTURE(text(v, "name"));
    CHECK_FALSE(record_id::parse(text(v, "text")).has_value());
  }
  for (const json_value &v : section("drbg")) {
    CAPTURE(text(v, "name"));
    deterministic_source src(number(v, "seed"), as_chars(hex(v, "stream_hex")));
    for (const json_value &block : v.find("blocks")->items) {
      std::array<std::uint8_t, sha256::digest_size> out{};
      REQUIRE(src.fill(out).has_value());
      CHECK(to_hex(out) == block.text);
    }
  }
  for (const json_value &v : section("trie_key")) {
    CAPTURE(text(v, "name"));
    sha256 h;
    h.update(std::string_view("slf-key/1"));
    h.update(hex(v, "id"));
    const sha256::value_type digest = h.finish();
    CHECK(to_hex(byte_span(digest).first(16)) == text(v, "key"));
  }
}

TEST_CASE("FAB-INV-ID-06: object ids and frames equal the vectors") {
  for (const json_value &v : section("oid")) {
    CAPTURE(text(v, "name"));
    const frame_header header{.major = static_cast<std::uint8_t>(number(v, "major")),
                              .minor = static_cast<std::uint8_t>(number(v, "minor")),
                              .kind = static_cast<object_kind>(number(v, "kind")),
                              .flags = static_cast<std::uint8_t>(number(v, "flags"))};
    const auto payload = hex(v, "payload");
    CHECK("sha256:" + hash_object(header, payload).to_hex() == text(v, "oid"));
    CHECK(frame_object(header, payload) == hex(v, "frame"));
    const auto parsed = parse_object(hex(v, "frame"));
    CHECK(parsed.has_value() == v.find("parses")->boolean);
    if (parsed) {
      CHECK("sha256:" + parsed->id.to_hex() == text(v, "oid"));
      CHECK(parsed->read_only() == v.find("read_only")->boolean);
    }
    if (header.kind == object_kind::record && header == frame_header{.kind = object_kind::record}) {
      CHECK("sha256:" + record_object_id(payload).to_hex() == text(v, "oid"));
    }
  }
}
