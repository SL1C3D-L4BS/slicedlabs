// FAB-INV-SER-03 (SL-ARCH-001 ARCH1-R31): the C++ encoder reproduces every accept vector of the
// SLB/1 golden corpus (hex, record oid, canonical text) and the C++ decoder rejects every
// must-reject vector with the reference rule and byte offset (tests/fixtures/fabric/slb).

#include "sl/fabric/codec.hpp"
#include "sl/fabric/store.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

json_value load(std::string_view rel) {
  const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/" + std::string(rel);
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), path);
  std::ostringstream s;
  s << in.rdbuf();
  return must(parse_json(s.str()));
}

std::string text_of(const json_value &v, std::string_view key) {
  const json_value *x = v.find(key);
  REQUIRE(x != nullptr);
  return x->text;
}

errc code_for(std::string_view rule) {
  static const std::map<std::string_view, errc> codes = {
      {"R-SER-VARINT", errc::decode_noncanonical},   {"R-SER-ORDER", errc::decode_noncanonical},
      {"R-SER-DUP", errc::decode_noncanonical},      {"R-SER-NEGZERO", errc::decode_noncanonical},
      {"R-SER-TRAILING", errc::decode_noncanonical}, {"R-SER-TRUNC", errc::decode_truncated},
      {"R-SER-LIMIT", errc::decode_limit},           {"R-SER-UTF8", errc::decode_bad_utf8},
      {"R-SER-NAN", errc::decode_bad_float},         {"R-SER-TAG", errc::decode_unknown_tag},
      {"R-SER-UNKNOWN", errc::decode_unknown_field},
  };
  const auto it = codes.find(rule);
  return it == codes.end() ? errc::internal : it->second;
}

} // namespace

TEST_CASE("FAB-INV-SER-03: every accept vector encodes to its hex and record oid") {
  const json_value accept = load("slb/accept.json");
  const json_value *vectors = accept.find("vectors");
  REQUIRE(vectors != nullptr);
  CHECK(vectors->items.size() == 228);
  std::size_t checked = 0;
  for (const json_value &vec : vectors->items) {
    const std::string name = text_of(vec, "name");
    CAPTURE(name);
    const value v = must(value_from_notation(*vec.find("value")));
    const auto bytes = must(encode(v));
    CHECK(to_hex(bytes) == text_of(vec, "hex"));
    CHECK("sha256:" + record_object_id(bytes).to_hex() == text_of(vec, "oid"));
    const value back = must(decode(bytes));
    CHECK(back == v);
    CHECK(must(encode(back)) == bytes);
    ++checked;
  }
  CHECK(checked == vectors->items.size());
}

TEST_CASE("FAB-INV-SER-03: every accept vector has its canonical text, both ways") {
  const json_value accept = load("slb/accept.json");
  const corpus_schemas schemas = must(load_corpus_schemas(load("slb/schemas.json")));
  // The only corpus types the C++ type system cannot express: a 65-bit int and field number 0.
  CHECK(schemas.skipped == std::vector<std::string>{"i65", "wide"});
  std::size_t texts = 0;
  std::size_t skipped = 0;
  for (const json_value &vec : accept.find("vectors")->items) {
    const std::string name = text_of(vec, "name");
    CAPTURE(name);
    const auto type = schemas.types.find(text_of(vec, "type"));
    if (type == schemas.types.end()) {
      ++skipped;
      continue;
    }
    const value v = must(value_from_notation(*vec.find("value")));
    const std::string want = text_of(vec, "text");
    CHECK(must(to_text(v, type->second, schemas.registry)) == want);
    const value parsed = must(from_text(want, type->second, schemas.registry));
    CHECK(parsed == v);
    ++texts;
  }
  CHECK(texts == 225);
  CHECK(skipped == 3);
}

TEST_CASE("FAB-INV-SER-03: every reject vector fails with its rule and offset") {
  const json_value reject = load("slb/reject.json");
  const corpus_schemas schemas = must(load_corpus_schemas(load("slb/schemas.json")));
  const json_value *vectors = reject.find("vectors");
  REQUIRE(vectors != nullptr);
  CHECK(vectors->items.size() == 278);
  std::set<std::string> rules;
  for (const json_value &vec : vectors->items) {
    const std::string name = text_of(vec, "name");
    const std::string rule = text_of(vec, "rule");
    const std::string offset = vec.find("offset")->text;
    CAPTURE(name);
    const auto bytes = must(from_hex(text_of(vec, "hex")));
    const std::string prefix = std::format("{} at offset {}", rule, offset);
    rules.insert(rule);
    if (text_of(vec, "target") == "frame") {
      const auto r = parse_object(bytes);
      REQUIRE_FALSE(r.has_value());
      CHECK_MESSAGE(r.error().message.starts_with(prefix), r.error().message);
      continue;
    }
    const bool strict = text_of(vec, "policy") == "strict";
    sl::result<value> r = decode(bytes);
    if (strict) {
      const auto type = schemas.types.find(text_of(vec, "type"));
      REQUIRE(type != schemas.types.end());
      CHECK(decode(bytes).has_value()); // preserve keeps the unknown field
      r = decode(bytes, unknown_policy::strict, type->second, schemas.registry);
    }
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == code_for(rule));
    CHECK_MESSAGE(r.error().message.starts_with(prefix + ":"), r.error().message);
  }
  CHECK(rules.size() == 12); // every R-SER rule of the reference
}
