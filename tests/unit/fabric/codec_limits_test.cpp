// FAB-INV-SER-09 (SL-ARCH-001 ARCH1-R30): depth 64, 2^24 items per container, 2^24-byte strings,
// bytes and order keys, and the 16 MiB record body are enforced before allocation: a tiny input
// that claims a huge count or length is refused (or found truncated) without a large allocation,
// and an over-limit value is refused by encode before its buffer exists.

#include "sl/fabric/codec.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

// Allocation probe: the largest single allocation while `armed` is set.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): the replaced global allocator
// can only report through globals
bool armed = false;
std::size_t largest = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

[[maybe_unused]] void *allocate(std::size_t n) {
  if (armed) {
    largest = std::max(largest, n);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): this is the replaceable global allocator itself
  void *p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) {
    std::abort();
  }
  return p;
}

// NOLINTNEXTLINE(cppcoreguidelines-no-malloc): the matching release of allocate()
[[maybe_unused]] void release(void *p) noexcept { std::free(p); }

} // namespace

// ThreadSanitizer ships its own global allocator and refuses a second definition at link time
// (duplicate symbol under mold), so the probe is compiled out there: the peak stays 0 and the limit
// checks still run.
#if defined(__SANITIZE_THREAD__)
#define SL_ALLOC_PROBE 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SL_ALLOC_PROBE 0
#else
#define SL_ALLOC_PROBE 1
#endif
#else
#define SL_ALLOC_PROBE 1
#endif
#if SL_ALLOC_PROBE
void *operator new(std::size_t n) { return allocate(n); }
void *operator new[](std::size_t n) { return allocate(n); }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, std::size_t /*n*/) noexcept { release(p); }
void operator delete[](void *p, std::size_t /*n*/) noexcept { release(p); }
#endif // SL_ALLOC_PROBE

namespace {

using namespace sl::fabric;
using sl::errc;

constexpr std::size_t small_allocation = 4096;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

std::vector<std::uint8_t> varint(std::uint64_t n) {
  std::array<std::uint8_t, max_uvarint_size> tmp{};
  const std::size_t len = encode_uvarint(n, tmp);
  return {tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(len)};
}

std::vector<std::uint8_t> cat(std::vector<std::uint8_t> a, const std::vector<std::uint8_t> &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

/// Decodes with the probe armed; returns the error and records the largest allocation.
sl::error decode_error(const std::vector<std::uint8_t> &input, std::size_t &peak) {
  largest = 0;
  armed = true;
  auto r = decode(input);
  armed = false;
  peak = largest;
  REQUIRE_FALSE(r.has_value());
  return r.error();
}

void check_refused(const std::vector<std::uint8_t> &input, errc code, const std::string &prefix) {
  std::size_t peak = 0;
  const sl::error e = decode_error(input, peak);
  CHECK(e.code == code);
  CHECK_MESSAGE(e.message.starts_with(prefix), e.message);
  CHECK(peak < small_allocation);
}

} // namespace

TEST_CASE("FAB-INV-SER-09: 64 nested containers decode, the 65th is refused at its tag") {
  for (const std::size_t depth : {std::size_t{64}, std::size_t{65}}) {
    std::vector<std::uint8_t> input;
    for (std::size_t i = 0; i < depth; ++i) {
      input.push_back(0x0B); // list of one item
      input.push_back(i + 1 < depth ? 0x01 : 0x00);
    }
    const auto r = decode(input);
    if (depth == 64) {
      REQUIRE(r.has_value());
      CHECK(r->depth() == 64);
      CHECK(must(encode(*r)) == input);
    } else {
      REQUIRE_FALSE(r.has_value());
      CHECK(r.error().code == errc::decode_limit);
      CHECK(r.error().message.starts_with("R-SER-LIMIT at offset 128:"));
    }
  }
  // Variants count as containers too.
  std::vector<std::uint8_t> variants;
  for (int i = 0; i < 65; ++i) {
    variants.push_back(0x0E);
    variants.push_back(0x00);
  }
  variants.push_back(0x01);
  check_refused(variants, errc::decode_limit, "R-SER-LIMIT at offset 128:");
}

TEST_CASE("FAB-INV-SER-09: huge counts and lengths fail before allocation") {
  const std::uint64_t over = max_container_items + 1;
  for (const std::uint8_t tag :
       {std::uint8_t{0x0B}, std::uint8_t{0x0C}, std::uint8_t{0x0D}, std::uint8_t{0x0F}}) {
    CAPTURE(tag);
    check_refused(cat({tag}, varint(over)), errc::decode_limit, "R-SER-LIMIT at offset 1:");
    // At the limit the count is accepted, and the missing items are a truncation: the decoder
    // must not reserve 2^24 items for a two-byte body.
    const auto at = cat({tag}, varint(max_container_items));
    check_refused(at, errc::decode_truncated,
                  "R-SER-TRUNC at offset " + std::to_string(at.size()) + ":");
  }
  for (const std::uint8_t tag : {std::uint8_t{0x07}, std::uint8_t{0x08}}) {
    check_refused(cat({tag}, varint(max_byte_length + 1)), errc::decode_limit,
                  "R-SER-LIMIT at offset 1:");
    check_refused(cat({tag}, varint(max_byte_length)), errc::decode_truncated, "R-SER-TRUNC");
  }
  // An oset order key over 2^24 bytes: offset of its length varint (tag, count, 16-byte id).
  auto oset = cat({0x0F, 0x01}, std::vector<std::uint8_t>(16, 0x05));
  check_refused(cat(oset, varint(max_byte_length + 1)), errc::decode_limit,
                "R-SER-LIMIT at offset 18:");
  // Field numbers and variant cases are u32.
  check_refused(cat({0x0D, 0x01}, varint(std::uint64_t{1} << 32U)), errc::decode_limit,
                "R-SER-LIMIT at offset 2:");
  check_refused(cat({0x0E}, varint(std::uint64_t{1} << 32U)), errc::decode_limit,
                "R-SER-LIMIT at offset 1:");
}

TEST_CASE("FAB-INV-SER-09: the record body is at most 16 MiB, both ways") {
  // A bytes value whose record is exactly 2^24 bytes: tag, 4-byte length, content.
  const std::vector<std::uint8_t> content(max_record_body - 5, 0xAB);
  const value at_limit = must(value::bytes(content));
  const auto bytes = must(encode(at_limit));
  CHECK(bytes.size() == max_record_body);
  CHECK(must(decode(bytes)) == at_limit);
  // One byte more is refused by encode, before the output buffer is allocated.
  const value over = must(value::bytes(std::vector<std::uint8_t>(max_record_body - 4, 0xAB)));
  largest = 0;
  armed = true;
  auto r = encode(over);
  armed = false;
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::encode_invalid);
  CHECK(r.error().message.starts_with("R-SER-LIMIT"));
  CHECK(largest < small_allocation);
  // Shared subtrees: three references to one 8 MiB node are a 24 MiB record.
  const value half = must(value::bytes(std::vector<std::uint8_t>(max_record_body / 2, 1)));
  largest = 0;
  armed = true;
  auto shared = encode(must(value::list({half, half, half})));
  armed = false;
  REQUIRE_FALSE(shared.has_value());
  CHECK(largest < small_allocation);
  // An input over 16 MiB is refused at offset 0 before anything is read.
  std::vector<std::uint8_t> big(max_record_body + 1, 0x01);
  check_refused(big, errc::decode_limit, "R-SER-LIMIT at offset 0:");
}
