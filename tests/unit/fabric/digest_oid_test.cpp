// FAB-INV-DG-02 (SL-ARCH-001 ARCH1-R09): the object id covers every frame header byte: changing
// major, minor, kind, flags or the payload length changes it; values agree with hashlib.

#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::fabric::byte_span;
using sl::fabric::digest;
using sl::fabric::frame_header;
using sl::fabric::hash_object;
using sl::fabric::object_kind;

byte_span as_bytes(std::string_view s) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): test bytes from a literal
  return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

// The ARCH1-R09 preimage built by hand: "slf-obj/1" 0x00, the four header bytes, the varint
// length bytes as given, the payload.
digest manual(std::array<std::uint8_t, 4> header, std::vector<std::uint8_t> length,
              byte_span payload) {
  sl::fabric::sha256 h;
  h.update(std::string_view{"slf-obj/1"});
  const std::array<std::uint8_t, 1> nul = {0};
  h.update(byte_span{nul});
  h.update(byte_span{header});
  h.update(byte_span{length});
  h.update(payload);
  return digest{.algo = sl::fabric::hash_algo::sha256, .bytes = h.finish()};
}

} // namespace

TEST_CASE("FAB-INV-DG-02: object ids equal the hashlib values") {
  CHECK(hash_object(frame_header{.kind = object_kind::blob}, {}).to_hex() ==
        "1145d20d4e79645aa6ebaaef178d37b9e94d5270ba85d116de9ab273d5b331c5");
  CHECK(hash_object(frame_header{.kind = object_kind::record}, as_bytes("abc")).to_hex() ==
        "87d274297364e9cd3f9fdcb06c586c7240c0e63526202f913d8e166532c05dd5");
  const std::vector<std::uint8_t> big(200, 'x'); // two-byte varint length (0xC8 0x01)
  CHECK(hash_object(frame_header{.kind = object_kind::blob}, big).to_hex() ==
        "b60cb8ce29e3239357d13682975310d35e27ad9eef8fee90fd77fdf527285e9c");
  CHECK(hash_object(frame_header{}, big) == manual({1, 0, 1, 0}, {0xC8, 0x01}, big));
  CHECK(hash_object(frame_header{}, big).algo == sl::fabric::hash_algo::sha256);
}

TEST_CASE("FAB-INV-DG-02: changing any frame header byte changes the object id") {
  const byte_span payload = as_bytes("payload");
  const frame_header base{.major = 1, .minor = 0, .kind = object_kind::record, .flags = 0};
  const digest ref = hash_object(base, payload);
  CHECK(ref == manual({1, 0, 2, 0}, {7}, payload));

  std::set<digest> seen{ref};
  for (unsigned v = 0; v < 256; ++v) {
    const auto b = static_cast<std::uint8_t>(v);
    frame_header h = base;
    h.major = b;
    seen.insert(hash_object(h, payload));
    h = base;
    h.minor = b;
    seen.insert(hash_object(h, payload));
    h = base;
    h.flags = b;
    seen.insert(hash_object(h, payload));
  }
  for (unsigned k = 1; k <= 10; ++k) {
    frame_header h = base;
    h.kind = static_cast<object_kind>(k);
    seen.insert(hash_object(h, payload));
  }
  // ref, 255 other values for each of major, minor and flags, 9 other kinds: all distinct.
  CHECK(seen.size() == 1 + (3 * 255) + 9);

  // The length byte is covered: the same header and payload with any other length byte in the
  // preimage gives a different id.
  for (unsigned v = 0; v < 256; ++v) {
    if (v != 7) {
      CHECK(manual({1, 0, 2, 0}, {static_cast<std::uint8_t>(v)}, payload) != ref);
    }
  }
  // Moving a byte between the payload and the header never collides: "ab" with flags 0 versus
  // "b" with flags 'a' differ in the length byte as well.
  frame_header shifted = base;
  shifted.flags = 'a';
  CHECK(hash_object(base, as_bytes("ab")) != hash_object(shifted, as_bytes("b")));
}

TEST_CASE("FAB-INV-DG-02: object kinds and frame constants") {
  CHECK(sl::fabric::frame_magic == std::array<std::uint8_t, 4>{'S', 'L', 'F', 'O'});
  CHECK(sl::fabric::frame_major == 1);
  CHECK(sl::fabric::frame_minor == 0);
  CHECK(sl::fabric::to_string(object_kind::schema_set) == "schema_set");
  CHECK(static_cast<int>(object_kind::schema_set) == 10);
  CHECK(sl::fabric::to_string(static_cast<object_kind>(11)) == "unknown");
  const digest d{};
  CHECK(d.to_hex() == std::string(64, '0'));
}
