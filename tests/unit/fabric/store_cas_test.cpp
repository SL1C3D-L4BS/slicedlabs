// FAB-INV-STO-01 (SL-ARCH-002 ARCH2-R02; SL-ARCH-001 ARCH1-R09, R10): get verifies the object id;
// a mismatch is object_corrupt and returns no bytes. Covers the SLFO frame codec it rests on
// (parse_object, frame_object) against the SLB/1 reference vectors, and verify's hash walk.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include "../../../engine/fabric/testing/store_faults.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::commit_batch;
using sl::fabric::digest;
using sl::fabric::memory_backend;
using sl::fabric::object;
using sl::fabric::object_kind;
using sl::fabric::parse_object;
using sl::fabric::ref_update;
using sl::fabric::testing::backdoor;
using sl::fabric::testing::store_faults;

std::vector<std::uint8_t> from_hex(std::string_view hex) {
  std::vector<std::uint8_t> out;
  const auto nibble = [](char c) -> unsigned {
    return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'a' + 10);
  };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
  }
  return out;
}

object text_object(object_kind kind, std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(kind, bytes);
}

struct oid_vector {
  std::string_view name;
  std::string_view frame;
  std::string_view oid; // empty: not openable
  bool read_only;
  errc code;
};

// tests/fixtures/fabric/hash/vectors.json, section oid (tools/fabric/slb_ref.py frame_oid).
constexpr std::string_view sample = "0e0d0201030102070673616d706c65";
std::vector<oid_vector> oid_vectors() {
  return {
      {.name = "kind_02",
       .frame = "534c464f01000200",
       .oid = "6e7b40e66cac5999650984bcfef36e9f7d1fd1d63e64af2a945ad269bd7f5742",
       .read_only = false,
       .code = errc::ok},
      {.name = "minor_1_read_only",
       .frame = "534c464f01010200",
       .oid = "e1327a4d0d67b3087d758feacfd1d7a1ab2a7cc95adb9da2301ca33165946aa2",
       .read_only = true,
       .code = errc::ok},
      {.name = "minor_255_read_only",
       .frame = "534c464f01ff0200",
       .oid = "30d0dd9f750f49f3c10afc3df7c4e28e7769f7d9307e83355b8e9fb89ac0d35f",
       .read_only = true,
       .code = errc::ok},
      {.name = "major_2_not_openable",
       .frame = "534c464f02000200",
       .oid = "",
       .read_only = false,
       .code = errc::format_unsupported},
      {.name = "flags_1_reserved",
       .frame = "534c464f01000201",
       .oid = "",
       .read_only = false,
       .code = errc::object_corrupt},
      {.name = "flags_80_reserved",
       .frame = "534c464f01000280",
       .oid = "",
       .read_only = false,
       .code = errc::object_corrupt},
  };
}

struct reject_vector {
  std::string_view name;
  std::string_view hex;
  std::string_view rule;
  std::size_t offset;
  errc code;
};

// tests/fixtures/fabric/slb/reject.json, every vector with target "frame".
std::vector<reject_vector> reject_vectors() {
  return {
      {.name = "varint_frame_length_overlong",
       .hex = "534c464f010002008000",
       .rule = "R-SER-VARINT",
       .offset = 8,
       .code = errc::decode_noncanonical},
      {.name = "trunc_frame_magic_partial",
       .hex = "534c",
       .rule = "R-SER-TRUNC",
       .offset = 2,
       .code = errc::decode_truncated},
      {.name = "trunc_frame_header_partial",
       .hex = "534c464f010002",
       .rule = "R-SER-TRUNC",
       .offset = 7,
       .code = errc::decode_truncated},
      {.name = "trunc_frame_no_length",
       .hex = "534c464f01000200",
       .rule = "R-SER-TRUNC",
       .offset = 8,
       .code = errc::decode_truncated},
      {.name = "trunc_frame_payload_short",
       .hex = "534c464f0100020005aabb",
       .rule = "R-SER-TRUNC",
       .offset = 11,
       .code = errc::decode_truncated},
      {.name = "trunc_frame_empty",
       .hex = "",
       .rule = "R-SER-TRUNC",
       .offset = 0,
       .code = errc::decode_truncated},
      {.name = "trailing_frame_byte",
       .hex = "534c464f010002000000",
       .rule = "R-SER-TRAILING",
       .offset = 9,
       .code = errc::decode_noncanonical},
      {.name = "limit_frame_payload_length_2p24_plus1",
       .hex = "534c464f0100020081808008",
       .rule = "R-SER-LIMIT",
       .offset = 8,
       .code = errc::decode_limit},
      {.name = "frame_bad_magic_last_byte",
       .hex = "534c46500100020000",
       .rule = "R-SER-FRAME",
       .offset = 0,
       .code = errc::object_corrupt},
      {.name = "frame_bad_magic_first_byte",
       .hex = "544c464f0100020000",
       .rule = "R-SER-FRAME",
       .offset = 0,
       .code = errc::object_corrupt},
      {.name = "frame_bad_magic_lowercase",
       .hex = "736c666f0100020000",
       .rule = "R-SER-FRAME",
       .offset = 0,
       .code = errc::object_corrupt},
      {.name = "frame_bad_magic_partial",
       .hex = "5358",
       .rule = "R-SER-FRAME",
       .offset = 0,
       .code = errc::object_corrupt},
      {.name = "frame_major_00",
       .hex = "534c464f0000020000",
       .rule = "R-SER-FRAME",
       .offset = 4,
       .code = errc::format_unsupported},
      {.name = "frame_major_02",
       .hex = "534c464f0200020000",
       .rule = "R-SER-FRAME",
       .offset = 4,
       .code = errc::format_unsupported},
      {.name = "frame_major_ff",
       .hex = "534c464fff00020000",
       .rule = "R-SER-FRAME",
       .offset = 4,
       .code = errc::format_unsupported},
      {.name = "frame_kind_00",
       .hex = "534c464f0100000000",
       .rule = "R-SER-FRAME",
       .offset = 6,
       .code = errc::object_corrupt},
      {.name = "frame_kind_0b",
       .hex = "534c464f01000b0000",
       .rule = "R-SER-FRAME",
       .offset = 6,
       .code = errc::object_corrupt},
      {.name = "frame_kind_ff",
       .hex = "534c464f0100ff0000",
       .rule = "R-SER-FRAME",
       .offset = 6,
       .code = errc::object_corrupt},
      {.name = "frame_flags_01",
       .hex = "534c464f0100020100",
       .rule = "R-SER-FRAME",
       .offset = 7,
       .code = errc::object_corrupt},
      {.name = "frame_flags_02",
       .hex = "534c464f0100020200",
       .rule = "R-SER-FRAME",
       .offset = 7,
       .code = errc::object_corrupt},
      {.name = "frame_flags_80",
       .hex = "534c464f0100028000",
       .rule = "R-SER-FRAME",
       .offset = 7,
       .code = errc::object_corrupt},
      {.name = "frame_flags_ff",
       .hex = "534c464f010002ff00",
       .rule = "R-SER-FRAME",
       .offset = 7,
       .code = errc::object_corrupt},
  };
}

commit_batch with_ref(object o) {
  commit_batch b;
  const digest id = o.id;
  b.objects.push_back(std::move(o));
  b.refs.push_back(ref_update{.name = "main", .expected = std::nullopt, .desired = id});
  return b;
}

} // namespace

TEST_CASE("FAB-INV-STO-01: frames round-trip and object ids equal the reference vectors") {
  for (const oid_vector &v : oid_vectors()) {
    CAPTURE(v.name);
    const std::vector<std::uint8_t> frame = from_hex(std::string(v.frame) + std::string(sample));
    const sl::result<object> parsed = parse_object(frame);
    if (v.code != errc::ok) {
      REQUIRE_FALSE(parsed.has_value());
      CHECK(parsed.error().code == v.code);
      continue;
    }
    REQUIRE(parsed.has_value());
    CHECK(parsed->id.to_hex() == v.oid);
    CHECK(parsed->id == sl::fabric::hash_object(parsed->header, parsed->payload));
    CHECK(parsed->read_only() == v.read_only);
    CHECK(parsed->payload == from_hex(sample.substr(2)));
    CHECK(sl::fabric::frame_object(parsed->header, parsed->payload) == frame);
    CHECK(sl::fabric::check_writable(parsed->header).has_value() == !v.read_only);
    if (v.read_only) {
      CHECK(sl::fabric::check_writable(parsed->header).error().code == errc::read_only_newer);
    }
  }
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{127}, std::size_t{128}, std::size_t{300}, std::size_t{20000}}) {
    const std::vector<std::uint8_t> payload(size, 0x5A);
    const std::vector<std::uint8_t> frame = sl::fabric::frame_object(object_kind::record, payload);
    const sl::result<object> parsed = parse_object(frame);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == sl::fabric::make_object(object_kind::record, payload));
  }
}

TEST_CASE("FAB-INV-STO-01: malformed frames are refused with the reference rule and offset") {
  for (const reject_vector &v : reject_vectors()) {
    CAPTURE(v.name);
    const sl::result<object> parsed = parse_object(from_hex(v.hex));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == v.code);
    const std::string expected = std::string(v.rule) + " at offset " + std::to_string(v.offset);
    CHECK(parsed.error().message.starts_with(expected));
  }
}

TEST_CASE("FAB-INV-STO-01: get verifies the object id and returns no bytes on a mismatch") {
  const auto faults = std::make_shared<store_faults>();
  memory_backend store(faults);
  const object o = text_object(object_kind::record, "the stored record body");
  REQUIRE(backdoor::commit(store, with_ref(o)).has_value());
  REQUIRE(store.get(o.id).has_value());

  const std::vector<std::uint8_t> frame = sl::fabric::frame_object(o.header, o.payload);
  std::size_t corrupt = 0;
  for (std::size_t at = 0; at < frame.size(); ++at) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      faults->flip_on_read(o.id, at, static_cast<std::uint8_t>(1U << bit));
      const sl::result<object> got = store.get(o.id);
      CAPTURE(at);
      CAPTURE(bit);
      REQUIRE_FALSE(got.has_value()); // no bytes come back
      CHECK(got.error().code == errc::object_corrupt);
      CHECK(got.error().message.find(o.id.to_hex()) != std::string::npos);
      corrupt += got.has_value() ? 0U : 1U;
    }
  }
  CHECK(corrupt == frame.size() * 8);
  faults->clear_flips();
  const sl::result<object> clean = store.get(o.id);
  REQUIRE(clean.has_value());
  CHECK(*clean == o);
}

TEST_CASE("FAB-INV-STO-01: a newer-minor frame under the requested id is still a mismatch") {
  const auto faults = std::make_shared<store_faults>();
  memory_backend store(faults);
  const object o = text_object(object_kind::blob, "payload");
  REQUIRE(backdoor::commit(store, with_ref(o)).has_value());
  faults->flip_on_read(o.id, 5, 0x01); // minor 0 -> 1: parses, read-only, other id
  const sl::result<object> got = store.get(o.id);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().code == errc::object_corrupt);
  CHECK(got.error().message.find("hash to") != std::string::npos);
}

TEST_CASE("FAB-INV-STO-01: verify reports a corrupt object once, from the hash walk") {
  const auto faults = std::make_shared<store_faults>();
  memory_backend store(faults);
  const object good = text_object(object_kind::blob, "good");
  const object bad = text_object(object_kind::blob, "bad");
  commit_batch batch = with_ref(bad);
  batch.objects.push_back(good);
  REQUIRE(backdoor::commit(store, std::move(batch)).has_value());
  REQUIRE(store.verify({}).value_or(sl::fabric::verify_report{}).ok());

  faults->flip_on_read(bad.id, 10, 0x80); // a payload byte
  const sl::result<sl::fabric::verify_report> report = store.verify({});
  REQUIRE(report.has_value());
  CHECK(report->objects_hashed == 2);
  CHECK(report->refs_walked == 1);
  REQUIRE(report->findings.size() == 1);
  CHECK(report->findings[0].code == errc::object_corrupt);
  CHECK(report->findings[0].id == bad.id);
  CHECK(report->findings[0].ref.empty());

  const sl::result<sl::fabric::verify_report> walk =
      store.verify({.hashes = false, .reachability = true, .links = nullptr});
  REQUIRE(walk.has_value());
  REQUIRE(walk->findings.size() == 1);
  CHECK(walk->findings[0].ref == "main");
}

TEST_CASE("FAB-INV-STO-01: a missing object is object_missing") {
  memory_backend store;
  const sl::result<object> got = store.get(text_object(object_kind::blob, "absent").id);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().code == errc::object_missing);
}
