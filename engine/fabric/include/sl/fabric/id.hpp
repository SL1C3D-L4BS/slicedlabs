#pragma once

// Record identity (WO-037, SL-ARCH-001 ARCH1-R01 to ARCH1-R04, ADR-0011).
//
// A record id is 16 bytes: byte 0 the id class, byte 1 the mint scheme, bytes 2-15 a 112-bit
// payload. Its text form is `<prefix>_<26 Crockford base32 characters>`: the 16 bytes read as a
// 130-bit big-endian number with two leading zero bits, so the first character is 0-7. Text is
// written and accepted in upper case only, so text and bytes are a bijection.
//
// Mint schemes (SL-ARCH-001 mint_schemes):
//   1 random      payload = 14 bytes from an injected id_source (mint)
//   2 derived     payload = SHA-256("slf-derive/1" || class || parent || u32le len(key) ||
//                                   key)[0:14]
//   3 well_known  payload bytes 2-11 zero, bytes 12-15 the ordinal big-endian
//   4 imported    payload = SHA-256("slf-import/1" || class || u32le len(system) || system ||
//                                   u32le len(external id) || external id)[0:14]
// An id holds no time, host, user or counter, and never changes class.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"

namespace sl::fabric {

/// Id classes (SL-ARCH-001 id_classes); 0 is none, 16-255 are reserved.
enum class id_class : std::uint8_t {
  none = 0,
  project = 1,
  schema = 2,
  type = 3,
  asset = 4,
  entity = 5,
  instance = 6,
  derived = 7,
  tx = 8,
  event = 9,
  actor = 10,
  package = 11,
  element = 12,
  namespace_ = 13, // "namespace" (a keyword in C++)
  operation = 14,
  import = 15,
};

/// Mint schemes (SL-ARCH-001 mint_schemes); 0 is none.
enum class mint_scheme : std::uint8_t {
  none = 0,
  random = 1,
  derived = 2,
  well_known = 3,
  imported = 4,
};

/// True for the 15 assigned classes (1-15).
[[nodiscard]] constexpr bool is_valid(id_class c) noexcept {
  const auto n = static_cast<std::uint8_t>(c);
  return n >= 1 && n <= 15;
}

/// True for the four assigned schemes (1-4).
[[nodiscard]] constexpr bool is_valid(mint_scheme s) noexcept {
  const auto n = static_cast<std::uint8_t>(s);
  return n >= 1 && n <= 4;
}

/// The class name ("asset", "namespace"); "none" for 0 and "unknown" for a reserved value.
[[nodiscard]] constexpr std::string_view to_string(id_class c) noexcept {
  switch (c) {
  case id_class::none:
    return "none";
  case id_class::project:
    return "project";
  case id_class::schema:
    return "schema";
  case id_class::type:
    return "type";
  case id_class::asset:
    return "asset";
  case id_class::entity:
    return "entity";
  case id_class::instance:
    return "instance";
  case id_class::derived:
    return "derived";
  case id_class::tx:
    return "tx";
  case id_class::event:
    return "event";
  case id_class::actor:
    return "actor";
  case id_class::package:
    return "package";
  case id_class::element:
    return "element";
  case id_class::namespace_:
    return "namespace";
  case id_class::operation:
    return "operation";
  case id_class::import:
    return "import";
  }
  return "unknown";
}

/// The three-letter text prefix ("ast"); empty for none and reserved values.
[[nodiscard]] constexpr std::string_view prefix(id_class c) noexcept {
  switch (c) {
  case id_class::project:
    return "prj";
  case id_class::schema:
    return "sch";
  case id_class::type:
    return "typ";
  case id_class::asset:
    return "ast";
  case id_class::entity:
    return "ent";
  case id_class::instance:
    return "ins";
  case id_class::derived:
    return "drv";
  case id_class::tx:
    return "txn";
  case id_class::event:
    return "evt";
  case id_class::actor:
    return "act";
  case id_class::package:
    return "pkg";
  case id_class::element:
    return "elm";
  case id_class::namespace_:
    return "nsp";
  case id_class::operation:
    return "ope";
  case id_class::import:
    return "imp";
  case id_class::none:
    return "";
  }
  return "";
}

/// The class whose prefix is `p`, if any.
[[nodiscard]] constexpr std::optional<id_class> class_from_prefix(std::string_view p) noexcept {
  for (std::uint8_t n = 1; n <= 15; ++n) {
    const auto c = static_cast<id_class>(n);
    if (prefix(c) == p) {
      return c;
    }
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_string(mint_scheme s) noexcept {
  switch (s) {
  case mint_scheme::none:
    return "none";
  case mint_scheme::random:
    return "random";
  case mint_scheme::derived:
    return "derived";
  case mint_scheme::well_known:
    return "well_known";
  case mint_scheme::imported:
    return "imported";
  }
  return "unknown";
}

namespace detail {
struct id_access;
} // namespace detail

/// A 16-byte record id. Default-constructed it is the null id (all zero); every other value has
/// an assigned class and scheme.
class record_id {
public:
  static constexpr std::size_t size = 16;
  static constexpr std::size_t payload_size = 14;
  static constexpr std::size_t text_size = 30; // 3 prefix + '_' + 26
  using bytes_type = std::array<std::uint8_t, size>;

  constexpr record_id() noexcept = default;

  /// `id_parse` unless `b` is 16 bytes with an assigned class and scheme.
  [[nodiscard]] static result<record_id> from_bytes(byte_span b);
  /// `id_parse` for a wrong length, separator, prefix or alphabet, a first character above 7, a
  /// prefix that disagrees with the class byte, or an unassigned scheme.
  [[nodiscard]] static result<record_id> parse(std::string_view text);

  [[nodiscard]] constexpr id_class cls() const noexcept { return static_cast<id_class>(bytes_[0]); }
  [[nodiscard]] constexpr mint_scheme scheme() const noexcept {
    return static_cast<mint_scheme>(bytes_[1]);
  }
  [[nodiscard]] constexpr const bytes_type &bytes() const noexcept { return bytes_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return bytes_ == bytes_type{}; }

  /// The canonical text form; "none" for the null id.
  [[nodiscard]] std::string to_string() const;

  friend constexpr auto operator<=>(const record_id &, const record_id &) noexcept = default;

private:
  friend struct detail::id_access;
  explicit constexpr record_id(const bytes_type &b) noexcept : bytes_(b) {}

  bytes_type bytes_{};
};

/// A record id statically bound to one class: ids of different classes do not convert into each
/// other, and `from` checks the class of an untyped id.
template <id_class C> class typed_id {
  static_assert(is_valid(C), "typed_id needs an assigned id class");

public:
  static constexpr id_class klass = C;

  constexpr typed_id() noexcept = default;

  /// `id_class_mismatch` unless `id` has class C.
  [[nodiscard]] static result<typed_id> from(const record_id &id) {
    if (id.cls() != C) {
      return make_error(errc::id_class_mismatch, std::string("expected a ") +
                                                     std::string(fabric::to_string(C)) +
                                                     " id, got " + id.to_string());
    }
    return typed_id{id};
  }

  /// record_id::parse, then from.
  [[nodiscard]] static result<typed_id> parse(std::string_view text) {
    return record_id::parse(text).and_then([](const record_id &id) { return from(id); });
  }

  [[nodiscard]] constexpr const record_id &id() const noexcept { return id_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return id_.is_null(); }
  [[nodiscard]] std::string to_string() const { return id_.to_string(); }

  friend constexpr auto operator<=>(const typed_id &, const typed_id &) noexcept = default;

private:
  explicit constexpr typed_id(const record_id &id) noexcept : id_(id) {}

  record_id id_;
};

using asset_id = typed_id<id_class::asset>;
using entity_id = typed_id<id_class::entity>;
using instance_id = typed_id<id_class::instance>;
using derived_id = typed_id<id_class::derived>;
using tx_id = typed_id<id_class::tx>;
using event_id = typed_id<id_class::event>;
using actor_id = typed_id<id_class::actor>;
using type_id = typed_id<id_class::type>;
using package_id = typed_id<id_class::package>;
using element_id = typed_id<id_class::element>;
using import_id = typed_id<id_class::import>;

/// Where random payloads come from. Production injects os_entropy_source, tests and replay a
/// deterministic_source (ADR-0011, ADR-0019).
class id_source {
public:
  virtual ~id_source() = default;
  /// Fills every byte of `out`, or fails with an error; a failure must not leave a usable value.
  [[nodiscard]] virtual result<void> fill(std::span<std::uint8_t> out) = 0;

protected:
  id_source() = default;
  id_source(const id_source &) = default;
  id_source &operator=(const id_source &) = default;
  id_source(id_source &&) = default;
  id_source &operator=(id_source &&) = default;
};

/// `entropy_failure` unless two 16-byte draws from `src` succeed, are non-zero and differ.
[[nodiscard]] result<void> entropy_self_test(id_source &src);

/// The operating system's entropy (getrandom on Linux).
class os_entropy_source final : public id_source {
public:
  /// The source, after entropy_self_test passed on it; `entropy_failure` otherwise.
  [[nodiscard]] static result<os_entropy_source> create();
  [[nodiscard]] result<void> fill(std::span<std::uint8_t> out) override;

private:
  os_entropy_source() = default;
};

/// SHA-256 counter mode: the byte stream is block 0, block 1, ... with
///   block i = SHA-256("slf-drbg/1" || seed u64le || u32le len(stream) || stream || i u64le).
/// fill never fails. The stream name must be shorter than 2^32 bytes.
class deterministic_source final : public id_source {
public:
  deterministic_source(std::uint64_t seed, std::string_view stream) noexcept;
  [[nodiscard]] result<void> fill(std::span<std::uint8_t> out) override;

private:
  sha256 prefix_;
  std::uint64_t next_block_ = 0;
  sha256::value_type block_{};
  std::size_t used_ = sha256::digest_size;
};

/// A random id of class `c`: `entropy_failure` when the source fails or yields an all-zero
/// payload, `invalid_argument` for an unassigned class.
[[nodiscard]] result<record_id> mint(id_class c, id_source &src);
/// Scheme 2. Precondition: `c` is assigned and `key` is shorter than 2^32 bytes (SL_ASSERT).
[[nodiscard]] record_id derive(id_class c, const record_id &parent, std::string_view key) noexcept;
/// Scheme 3. Precondition: `c` is assigned (SL_ASSERT).
[[nodiscard]] record_id well_known(id_class c, std::uint32_t ordinal) noexcept;
/// Scheme 4. Precondition: `c` is assigned, both strings are shorter than 2^32 bytes (SL_ASSERT).
[[nodiscard]] record_id imported(id_class c, std::string_view system,
                                 std::string_view external_id) noexcept;

} // namespace sl::fabric
