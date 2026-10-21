// RT-INV-DET-02 (SL-ARCH-003 ARCH3-R088, `limits` canonical_nan_f32/f64, ADR-0026): state_hasher
// equals the 40 committed cases of tests/golden/runtime/hash_vectors.json (every width, +-0.0, NaN
// payloads, bytes, text and mixed sequences; the oracle is an independent stdlib Python hashlib
// encoder recorded in the WO-084 deviation file); -0.0 and +0.0 hash equal, every NaN payload
// hashes equal, and u32(5) and u64(5) differ because the width is encoded.

#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace det = sl::runtime::det;

std::string read_golden(const char *name) {
  std::ifstream in(std::string(SL_RUNTIME_SOURCE_DIR "/tests/golden/runtime/") + name);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

// The quoted string, or the array of quoted strings, after "key": on one line (no escapes).
std::vector<std::string> field(std::string_view line, std::string_view key) {
  std::vector<std::string> out;
  const std::string needle = "\"" + std::string(key) + "\": ";
  const std::size_t at = line.find(needle);
  if (at == std::string_view::npos) {
    return out;
  }
  std::size_t pos = at + needle.size();
  const bool array = line[pos] == '[';
  for (;;) {
    const std::size_t open = line.find('"', pos);
    const std::size_t stop = array ? line.find(']', pos) : std::string_view::npos;
    if (open == std::string_view::npos || (array && open > stop)) {
      break;
    }
    const std::size_t close = line.find('"', open + 1);
    out.emplace_back(line.substr(open + 1, close - open - 1));
    pos = close + 1;
    if (!array) {
      break;
    }
  }
  return out;
}

template <class T> T parse(std::string_view text) {
  int base = 10;
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
    base = 16;
  }
  T v{};
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v, base);
  REQUIRE(ec == std::errc{});
  REQUIRE(ptr == text.data() + text.size());
  return v;
}

std::vector<std::byte> hex_bytes(std::string_view hex) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(parse<unsigned>("0x" + std::string(hex.substr(i, 2)))));
  }
  return out;
}

void apply(det::state_hasher &h, std::string_view op, std::string_view v) {
  if (op == "u8") {
    h.u8(parse<std::uint8_t>(v));
  } else if (op == "u16") {
    h.u16(parse<std::uint16_t>(v));
  } else if (op == "u32") {
    h.u32(parse<std::uint32_t>(v));
  } else if (op == "u64") {
    h.u64(parse<std::uint64_t>(v));
  } else if (op == "i8") {
    h.i8(parse<std::int8_t>(v));
  } else if (op == "i16") {
    h.i16(parse<std::int16_t>(v));
  } else if (op == "i32") {
    h.i32(parse<std::int32_t>(v));
  } else if (op == "i64") {
    h.i64(parse<std::int64_t>(v));
  } else if (op == "bool") {
    h.boolean(v == "true");
  } else if (op == "f32") {
    h.f32(std::bit_cast<float>(parse<std::uint32_t>(v)));
  } else if (op == "f64") {
    h.f64(std::bit_cast<double>(parse<std::uint64_t>(v)));
  } else if (op == "bytes") {
    h.bytes(hex_bytes(v));
  } else if (op == "str") {
    h.str(v);
  } else {
    FAIL("unknown op " << op);
  }
}

std::string digest_of(std::string_view op, std::string_view v) {
  det::state_hasher h;
  apply(h, op, v);
  return h.finish().to_hex();
}

} // namespace

TEST_CASE("RT-INV-DET-02: state_hasher equals the 40 cases of hash_vectors.json") {
  const std::string text = read_golden("hash_vectors.json");
  REQUIRE_FALSE(text.empty());
  std::istringstream lines(text);
  int count = 0;
  for (std::string line; std::getline(lines, line);) {
    if (!line.contains("\"name\"")) {
      continue;
    }
    const std::string name = field(line, "name").at(0);
    const std::vector<std::string> ops = field(line, "ops");
    const std::string expected = field(line, "sha256").at(0);
    CAPTURE(name);
    REQUIRE(ops.size() % 2 == 0);
    det::state_hasher h;
    for (std::size_t i = 0; i < ops.size(); i += 2) {
      apply(h, ops[i], ops[i + 1]);
    }
    CHECK(h.finish().to_hex() == expected);
    CHECK(h.finish().algo == sl::fabric::hash_algo::sha256);
    ++count;
  }
  CHECK(count == 40);
}

TEST_CASE("RT-INV-DET-02: -0.0 and +0.0 hash equal; every NaN payload hashes equal") {
  CHECK(digest_of("f32", "0x80000000") == digest_of("f32", "0x00000000"));
  CHECK(digest_of("f64", "0x8000000000000000") == digest_of("f64", "0x0000000000000000"));
  const std::string nan32 = digest_of("f32", "0x7fc00000");
  for (const char *bits : {"0x7f800001", "0x7fffffff", "0xffc00000", "0xff800001", "0x7fa00000"}) {
    CHECK(digest_of("f32", bits) == nan32);
  }
  const std::string nan64 = digest_of("f64", "0x7ff8000000000000");
  for (const char *bits :
       {"0x7ff0000000000001", "0x7fffffffffffffff", "0xfff8000000000000", "0xfff0000000000001"}) {
    CHECK(digest_of("f64", bits) == nan64);
  }
  det::state_hasher a;
  a.f64(std::numeric_limits<double>::quiet_NaN());
  det::state_hasher b;
  b.f64(-std::numeric_limits<double>::quiet_NaN());
  CHECK(a.finish() == b.finish());
  det::state_hasher z;
  z.f32(-0.0F);
  det::state_hasher p;
  p.f32(0.0F);
  CHECK(z.finish() == p.finish());
  // Infinities and ordinary values stay distinct.
  CHECK(digest_of("f32", "0x7f800000") != nan32);
  CHECK(digest_of("f64", "0xfff0000000000000") != digest_of("f64", "0x7ff0000000000000"));
}

TEST_CASE("RT-INV-DET-02: u32(5) and u64(5) differ; widths, signs and kinds are encoded") {
  CHECK(digest_of("u32", "5") != digest_of("u64", "5"));
  CHECK(digest_of("u8", "5") != digest_of("i8", "5"));
  CHECK(digest_of("u16", "5") != digest_of("u32", "5"));
  CHECK(digest_of("bytes", "616263") != digest_of("str", "abc"));
  CHECK(digest_of("bool", "true") != digest_of("u8", "1"));
  det::state_hasher h;
  h.u32(1);
  const auto first = h.finish();
  CHECK(h.finish() == first); // finish leaves the state unchanged
  h.u32(2);
  CHECK(h.finish() != first);
}
