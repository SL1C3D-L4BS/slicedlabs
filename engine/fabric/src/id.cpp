#include "sl/fabric/id.hpp"

#include "sl/core/assert.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>

#ifdef __linux__
#include <sys/random.h>
#endif

namespace sl::fabric {

namespace detail {

struct id_access {
  static record_id make(const record_id::bytes_type &b) noexcept { return record_id{b}; }
};

} // namespace detail

namespace {

constexpr std::string_view crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t text_digits = 26;

/// The value of a Crockford digit (upper case only), or -1.
constexpr int digit_value(char ch) noexcept {
  const std::size_t i = crockford.find(ch);
  return i == std::string_view::npos ? -1 : static_cast<int>(i);
}

std::unexpected<error> parse_error(std::string_view text, std::string_view why) {
  return make_error(errc::id_parse, std::format("id '{}': {}", text, why));
}

void put_u32le(sha256 &h, std::size_t n) noexcept {
  const auto v = static_cast<std::uint32_t>(n);
  const std::array<std::uint8_t, 4> b = {
      static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8U),
      static_cast<std::uint8_t>(v >> 16U), static_cast<std::uint8_t>(v >> 24U)};
  h.update(byte_span{b});
}

void put_u64le(sha256 &h, std::uint64_t v) noexcept {
  std::array<std::uint8_t, 8> b{};
  for (std::uint8_t &x : b) {
    x = static_cast<std::uint8_t>(v);
    v >>= 8U;
  }
  h.update(byte_span{b});
}

std::uint64_t load_be64(byte_span b) noexcept {
  std::uint64_t v = 0;
  for (const std::uint8_t x : b) {
    v = (v << 8U) | x;
  }
  return v;
}

void store_be64(std::uint64_t v, std::span<std::uint8_t> out) noexcept {
  for (std::uint8_t &x : std::views::reverse(out)) {
    x = static_cast<std::uint8_t>(v);
    v >>= 8U;
  }
}

/// Bits [shift, shift + 5) of the 128-bit number hi:lo.
constexpr unsigned digit_at(std::uint64_t hi, std::uint64_t lo, unsigned shift) noexcept {
  std::uint64_t v = 0;
  if (shift >= 64) {
    v = hi >> (shift - 64U);
  } else if (shift + 5U <= 64U) {
    v = lo >> shift;
  } else {
    v = (lo >> shift) | (hi << (64U - shift));
  }
  return static_cast<unsigned>(v & 0x1FU);
}

constexpr bool fits_u32(std::size_t n) noexcept {
  return n <= std::numeric_limits<std::uint32_t>::max();
}

record_id from_hash(id_class c, mint_scheme s, const sha256 &h) noexcept {
  const sha256::value_type d = h.finish();
  record_id::bytes_type b{};
  b[0] = static_cast<std::uint8_t>(c);
  b[1] = static_cast<std::uint8_t>(s);
  std::copy_n(d.begin(), record_id::payload_size, b.begin() + 2);
  return detail::id_access::make(b);
}

} // namespace

result<record_id> record_id::from_bytes(byte_span b) {
  if (b.size() != size) {
    return make_error(errc::id_parse,
                      std::format("record id needs {} bytes, got {}", size, b.size()));
  }
  bytes_type out{};
  std::ranges::copy(b, out.begin());
  if (!is_valid(static_cast<id_class>(out[0]))) {
    return make_error(errc::id_parse, std::format("record id class {} is not assigned", out[0]));
  }
  if (!is_valid(static_cast<mint_scheme>(out[1]))) {
    return make_error(errc::id_parse, std::format("record id scheme {} is not assigned", out[1]));
  }
  return record_id{out};
}

std::string record_id::to_string() const {
  if (!is_valid(cls())) {
    return "none";
  }
  std::string out;
  out.reserve(text_size);
  out.append(prefix(cls()));
  out.push_back('_');
  const byte_span all{bytes_};
  const std::uint64_t hi = load_be64(all.first(8));
  const std::uint64_t lo = load_be64(all.last(8));
  for (std::size_t i = 0; i < text_digits; ++i) {
    const auto shift = static_cast<unsigned>(5 * (text_digits - 1 - i));
    out.push_back(crockford[digit_at(hi, lo, shift)]);
  }
  return out;
}

result<record_id> record_id::parse(std::string_view text) {
  if (text.size() != text_size) {
    return parse_error(text, std::format("length {}, want {}", text.size(), text_size));
  }
  if (text[3] != '_') {
    return parse_error(text, "no '_' after the three-letter prefix");
  }
  const std::optional<id_class> c = class_from_prefix(text.substr(0, 3));
  if (!c) {
    return parse_error(text, "unknown prefix");
  }
  const std::string_view digits = text.substr(4);
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  std::size_t at = 4;
  for (const char ch : digits) {
    const int v = digit_value(ch);
    if (v < 0) {
      return parse_error(text, std::format("character {} is not upper-case Crockford base32", at));
    }
    if (at == 4 && v > 7) {
      return parse_error(text, "first digit above 7 (the id has 128 bits)");
    }
    // 3 bits from the first digit and 5 from each of the other 25 fill exactly 128 bits.
    hi = (hi << 5U) | (lo >> 59U);
    lo = (lo << 5U) | static_cast<std::uint64_t>(v);
    ++at;
  }
  bytes_type out{};
  store_be64(hi, std::span{out}.first<8>());
  store_be64(lo, std::span{out}.last<8>());
  if (static_cast<id_class>(out[0]) != *c) {
    return parse_error(text,
                       std::format("prefix {} disagrees with class byte {}", prefix(*c), out[0]));
  }
  if (!is_valid(static_cast<mint_scheme>(out[1]))) {
    return parse_error(text, std::format("scheme {} is not assigned", out[1]));
  }
  return record_id{out};
}

result<void> entropy_self_test(id_source &src) {
  std::array<std::uint8_t, record_id::size> a{};
  std::array<std::uint8_t, record_id::size> b{};
  if (auto r = src.fill(a); !r) {
    return make_error(errc::entropy_failure, "self-test: first draw failed: " + r.error().message);
  }
  if (auto r = src.fill(b); !r) {
    return make_error(errc::entropy_failure, "self-test: second draw failed: " + r.error().message);
  }
  constexpr std::array<std::uint8_t, record_id::size> zero{};
  if (a == zero || b == zero) {
    return make_error(errc::entropy_failure, "self-test: a draw was all zero");
  }
  if (a == b) {
    return make_error(errc::entropy_failure, "self-test: two draws were equal");
  }
  return {};
}

result<os_entropy_source> os_entropy_source::create() {
  os_entropy_source src;
  if (auto r = entropy_self_test(src); !r) {
    return std::unexpected(std::move(r).error());
  }
  return src;
}

result<void> os_entropy_source::fill(std::span<std::uint8_t> out) {
#ifdef __linux__
  std::size_t done = 0;
  while (done < out.size()) {
    const auto rest = out.subspan(done);
    const ssize_t n = ::getrandom(rest.data(), rest.size(), 0);
    if (n < 0) {
      const int err = errno;
      if (err == EINTR) {
        continue;
      }
      return make_error(errc::entropy_failure, std::format("getrandom: {}", std::strerror(err)));
    }
    done += static_cast<std::size_t>(n);
  }
  return {};
#else
  static_cast<void>(out);
  return make_error(errc::entropy_failure, "no operating-system entropy source on this platform");
#endif
}

deterministic_source::deterministic_source(std::uint64_t seed, std::string_view stream) noexcept {
  SL_ASSERT(fits_u32(stream.size()), "deterministic_source: stream name too long");
  prefix_.update(std::string_view{"slf-drbg/1"});
  put_u64le(prefix_, seed);
  put_u32le(prefix_, stream.size());
  prefix_.update(stream);
}

result<void> deterministic_source::fill(std::span<std::uint8_t> out) {
  std::size_t done = 0;
  while (done < out.size()) {
    if (used_ == block_.size()) {
      sha256 h = prefix_;
      put_u64le(h, next_block_++);
      block_ = h.finish();
      used_ = 0;
    }
    const std::size_t take = std::min(block_.size() - used_, out.size() - done);
    std::copy_n(block_.begin() + static_cast<std::ptrdiff_t>(used_), take,
                out.begin() + static_cast<std::ptrdiff_t>(done));
    used_ += take;
    done += take;
  }
  return {};
}

result<record_id> mint(id_class c, id_source &src) {
  if (!is_valid(c)) {
    return make_error(errc::invalid_argument,
                      std::format("mint: id class {} is not assigned", static_cast<unsigned>(c)));
  }
  std::array<std::uint8_t, record_id::payload_size> payload{};
  if (auto r = src.fill(payload); !r) {
    return make_error(errc::entropy_failure, "mint: id_source failed: " + r.error().message);
  }
  if (std::ranges::all_of(payload, [](std::uint8_t b) { return b == 0; })) {
    return make_error(errc::entropy_failure, "mint: id_source returned an all-zero payload");
  }
  record_id::bytes_type b{};
  b[0] = static_cast<std::uint8_t>(c);
  b[1] = static_cast<std::uint8_t>(mint_scheme::random);
  std::ranges::copy(payload, b.begin() + 2);
  return detail::id_access::make(b);
}

record_id derive(id_class c, const record_id &parent, std::string_view key) noexcept {
  SL_ASSERT(is_valid(c), "derive: unassigned id class");
  SL_ASSERT(fits_u32(key.size()), "derive: key too long");
  sha256 h;
  h.update(std::string_view{"slf-derive/1"});
  const std::array<std::uint8_t, 1> cls = {static_cast<std::uint8_t>(c)};
  h.update(byte_span{cls});
  h.update(byte_span{parent.bytes()});
  put_u32le(h, key.size());
  h.update(key);
  return from_hash(c, mint_scheme::derived, h);
}

record_id well_known(id_class c, std::uint32_t ordinal) noexcept {
  SL_ASSERT(is_valid(c), "well_known: unassigned id class");
  record_id::bytes_type b{};
  b[0] = static_cast<std::uint8_t>(c);
  b[1] = static_cast<std::uint8_t>(mint_scheme::well_known);
  b[12] = static_cast<std::uint8_t>(ordinal >> 24U);
  b[13] = static_cast<std::uint8_t>(ordinal >> 16U);
  b[14] = static_cast<std::uint8_t>(ordinal >> 8U);
  b[15] = static_cast<std::uint8_t>(ordinal);
  return detail::id_access::make(b);
}

record_id imported(id_class c, std::string_view system, std::string_view external_id) noexcept {
  SL_ASSERT(is_valid(c), "imported: unassigned id class");
  SL_ASSERT(fits_u32(system.size()) && fits_u32(external_id.size()),
            "imported: system or external id too long");
  sha256 h;
  h.update(std::string_view{"slf-import/1"});
  const std::array<std::uint8_t, 1> cls = {static_cast<std::uint8_t>(c)};
  h.update(byte_span{cls});
  put_u32le(h, system.size());
  h.update(system);
  put_u32le(h, external_id.size());
  h.update(external_id);
  return from_hash(c, mint_scheme::imported, h);
}

} // namespace sl::fabric
