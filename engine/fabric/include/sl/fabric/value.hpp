#pragma once

// Canonical values (WO-039, SL-ARCH-001 ARCH1-R14 to ARCH1-R16, ADR-0012).
//
// A value is an immutable tree of shared nodes: copying a value copies one pointer, and `with`,
// `without`, `insert_element` and `move_element` return a new value that shares every untouched
// subtree with the original and leave the original bit-identical (ARCH1-R16).
//
// The value model is exactly the SLB/1 model (tools/fabric/slb_ref.py): bool, uint (0..2^64-1),
// nint (-2^64..-1, held as the wire magnitude m with value -(m+1)), f32, f64, string, bytes, id
// (16 raw bytes), digest (algo byte and 32 bytes), list, map, struct, variant and oset. Every
// value can be encoded; a constructor refuses with `encode_invalid` (naming the R-SER rule) what
// SLB/1 cannot hold: a non-finite float (R-SER-NAN), ill-formed UTF-8 or a NUL in a string
// (R-SER-UTF8), a string, bytes or order key longer than 2^24 bytes, a container of more than 2^24
// items or nested deeper than 64 containers (R-SER-LIMIT), and a repeated map key, struct field
// number or oset element id (R-SER-DUP). -0.0 is stored as +0.0.
//
// Containers keep the canonical order of ADR-0012, so iteration is deterministic and equals the
// encoding order: map entries ascending by encoded key bytes, struct fields ascending by number,
// oset entries ascending by (order key bytes, element id).
//
// A path addresses a value by field number, map key or element id, never by position
// (ARCH2-R03). A field segment also steps into a variant whose active case has that number. A list
// is atomic: no path enters one (ARCH1-R15).
//
// Presence (ARCH1-R14): a struct holds exactly the fields that were written. An absent field and a
// field present with its default are different values; defaults are applied by readers
// (type.hpp read_field) and never stored.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"

namespace sl::fabric {

/// The 16 raw bytes of an id value or an oset element id (any bytes: SLB/1 does not check ids).
using id_bytes = record_id::bytes_type;

/// SLB/1 limits (ARCH1-R30).
inline constexpr std::size_t max_value_depth = 64;           // nested containers
inline constexpr std::size_t max_container_items = 1U << 24; // items per container
inline constexpr std::size_t max_byte_length = 1U << 24;     // string, bytes, order key

/// Value kinds; the number is the SLB/1 tag (bool covers the tags 0x01 false and 0x02 true).
enum class value_kind : std::uint8_t {
  bool_ = 1,
  uint = 3,
  nint = 4,
  f32 = 5,
  f64 = 6,
  string = 7,
  bytes = 8,
  id = 9,
  digest = 10,
  list = 11,
  map = 12,
  struct_ = 13,
  variant = 14,
  oset = 15,
};

/// "bool", "uint", "struct", ...
[[nodiscard]] std::string_view to_string(value_kind k) noexcept;

/// True for list, map, struct, variant and oset.
[[nodiscard]] constexpr bool is_container(value_kind k) noexcept {
  return k == value_kind::list || k == value_kind::map || k == value_kind::struct_ ||
         k == value_kind::variant || k == value_kind::oset;
}

/// True for the kinds a map key may have: uint, nint, string and id.
[[nodiscard]] constexpr bool is_key_kind(value_kind k) noexcept {
  return k == value_kind::uint || k == value_kind::nint || k == value_kind::string ||
         k == value_kind::id;
}

namespace detail {
struct value_node;
struct value_access;
} // namespace detail

struct field;
struct map_entry;
struct oset_entry;
class path_segment;
using path = std::vector<path_segment>;

class value {
public:
  /// The bool false.
  value() noexcept = default;

  // Scalars.
  [[nodiscard]] static value boolean(bool b);
  [[nodiscard]] static value uint(std::uint64_t v);
  /// uint for v >= 0, nint otherwise.
  [[nodiscard]] static value integer(std::int64_t v);
  /// The negative integer -(magnitude + 1): the SLB/1 nint payload, so -1 is nint(0) and -2^64 is
  /// nint(2^64 - 1).
  [[nodiscard]] static value nint(std::uint64_t magnitude);
  /// `encode_invalid` (R-SER-NAN) for NaN or an infinity; -0.0 becomes +0.0.
  [[nodiscard]] static result<value> f32(float v);
  [[nodiscard]] static result<value> f64(double v);
  /// `encode_invalid`: R-SER-UTF8 for ill-formed UTF-8 or a NUL byte, R-SER-LIMIT over 2^24 bytes.
  [[nodiscard]] static result<value> string(std::string_view text);
  /// `encode_invalid` (R-SER-LIMIT) over 2^24 bytes.
  [[nodiscard]] static result<value> bytes(byte_span b);
  [[nodiscard]] static value id(const record_id &id);
  [[nodiscard]] static value id(const id_bytes &raw);
  [[nodiscard]] static value digest(const fabric::digest &d);

  // Containers: `encode_invalid` (R-SER-LIMIT) over 2^24 items or 64 nested containers.
  [[nodiscard]] static result<value> list(std::vector<value> items);
  /// Sorts by encoded key; R-SER-TAG for a key that is not uint, nint, string or id, R-SER-DUP
  /// for a repeated key.
  [[nodiscard]] static result<value> map(std::vector<map_entry> entries);
  /// Sorts by field number; R-SER-DUP for a repeated number.
  [[nodiscard]] static result<value> struct_(std::vector<field> fields);
  [[nodiscard]] static result<value> variant(std::uint32_t case_no, value payload);
  /// Sorts by (order key, element id); R-SER-DUP for a repeated element id. Order keys are not
  /// judged here: any bytes encode, validity is a schema rule (type.hpp is_valid_order_key).
  [[nodiscard]] static result<value> oset(std::vector<oset_entry> entries);

  [[nodiscard]] value_kind kind() const noexcept;
  /// Nested containers: 0 for a scalar, 1 + the deepest child for a container.
  [[nodiscard]] std::size_t depth() const noexcept;
  /// The address of the shared node (null for the bool false): equal addresses mean the same
  /// subtree, which is how structural sharing is observed (FAB-INV-TY-03).
  [[nodiscard]] const void *node_address() const noexcept { return node_.get(); }

  // Accessors. Precondition: kind() is the accessor's kind (SL_ASSERT).
  [[nodiscard]] bool as_bool() const noexcept;
  [[nodiscard]] std::uint64_t as_uint() const noexcept;
  /// The wire magnitude m of the nint -(m + 1).
  [[nodiscard]] std::uint64_t nint_magnitude() const noexcept;
  [[nodiscard]] float as_f32() const noexcept;
  [[nodiscard]] double as_f64() const noexcept;
  [[nodiscard]] std::string_view as_string() const noexcept;
  [[nodiscard]] byte_span as_bytes() const noexcept;
  [[nodiscard]] const id_bytes &as_id_bytes() const noexcept;
  [[nodiscard]] fabric::digest as_digest() const noexcept;
  [[nodiscard]] std::span<const value> items() const noexcept;
  [[nodiscard]] std::span<const map_entry> entries() const noexcept;
  [[nodiscard]] std::span<const field> fields() const noexcept;
  [[nodiscard]] std::uint32_t case_no() const noexcept;
  [[nodiscard]] const value &payload() const noexcept;
  [[nodiscard]] std::span<const oset_entry> elements() const noexcept;

  /// The integer as int64 when it is a uint or nint in range; nullopt otherwise.
  [[nodiscard]] std::optional<std::int64_t> as_i64() const noexcept;
  /// The id as a record id; `id_parse` unless it has an assigned class and scheme.
  [[nodiscard]] result<record_id> as_record_id() const;

  // Lookups, null when absent or when the kind does not match.
  [[nodiscard]] const value *find_field(std::uint32_t no) const noexcept;
  [[nodiscard]] const value *find_key(const value &key) const noexcept;
  [[nodiscard]] const oset_entry *find_element(const id_bytes &element) const noexcept;
  [[nodiscard]] bool has_field(std::uint32_t no) const noexcept {
    return find_field(no) != nullptr;
  }

  // Paths (the brief's `at(path)` is spelled `get`: the nothrow lint, N-AT, flags `.at(` calls).
  /// The value at `path`: `type_mismatch` when a segment does not fit the value it is applied to,
  /// `not_found` when the field, key or element is absent.
  [[nodiscard]] result<value> get(const path &p) const;
  /// A copy with the value at `path` set to `v`. The last segment may name an absent struct field
  /// or map key (it is inserted) but an element must exist (insert_element adds one); every
  /// earlier segment must exist. The empty path returns `v`.
  [[nodiscard]] result<value> with(const path &p, value v) const;
  /// A copy without the field, key or element `path` names; an absent last segment returns this
  /// value unchanged (same node). The empty path and a variant payload are `type_mismatch`.
  [[nodiscard]] result<value> without(const path &p) const;
  /// A copy whose oset at `oset_path` gains `element` at `order_key`; `already_exists` when the
  /// element id is present.
  [[nodiscard]] result<value> insert_element(const path &oset_path, const id_bytes &element,
                                             const std::vector<std::uint8_t> &order_key,
                                             value v) const;
  /// A copy whose oset at `oset_path` has `element` moved to `order_key`.
  [[nodiscard]] result<value> move_element(const path &oset_path, const id_bytes &element,
                                           const std::vector<std::uint8_t> &order_key) const;

  /// Structural equality (floats compare by bits; -0.0 and NaN cannot occur).
  friend bool operator==(const value &a, const value &b) noexcept;

private:
  explicit value(std::shared_ptr<const detail::value_node> n) noexcept : node_(std::move(n)) {}
  friend struct detail::value_access;

  std::shared_ptr<const detail::value_node> node_;
};

/// A struct field: stable number and value.
struct field {
  std::uint32_t no = 0;
  value v;

  friend bool operator==(const field &, const field &) noexcept = default;
};

/// A map entry; the key is a uint, nint, string or id value.
struct map_entry {
  value key;
  value v;

  friend bool operator==(const map_entry &, const map_entry &) noexcept = default;
};

/// An oset entry: element id, fractional order key bytes, value (ARCH1-R15).
struct oset_entry {
  id_bytes element{};
  std::vector<std::uint8_t> order_key;
  value v;

  friend bool operator==(const oset_entry &, const oset_entry &) noexcept = default;
};

/// The canonical order of map keys: by their SLB/1 encoding (tag, then payload), so every uint
/// sorts before every nint, string and id, and "aa" sorts after "b". Keys must be key kinds.
[[nodiscard]] std::strong_ordering compare_keys(const value &a, const value &b) noexcept;

/// One step of a path: a struct field number (or variant case), a map key or an oset element id.
class path_segment {
public:
  enum class kind_type : std::uint8_t { field = 1, key = 2, element = 3 };

  [[nodiscard]] static path_segment field(std::uint32_t no) noexcept;
  /// `type_mismatch` unless `k` is a uint, nint, string or id.
  [[nodiscard]] static result<path_segment> key(value k);
  [[nodiscard]] static path_segment element(const id_bytes &element) noexcept;
  [[nodiscard]] static path_segment element(const record_id &element) noexcept;

  [[nodiscard]] kind_type kind() const noexcept { return kind_; }
  [[nodiscard]] std::uint32_t field_no() const noexcept { return no_; }
  [[nodiscard]] const value &key_value() const noexcept { return key_; }
  [[nodiscard]] const id_bytes &element_id() const noexcept { return element_; }

  friend bool operator==(const path_segment &a, const path_segment &b) noexcept;
  /// Fields before keys before elements; then field number, compare_keys or element bytes.
  friend std::strong_ordering operator<=>(const path_segment &a, const path_segment &b) noexcept;

private:
  path_segment() noexcept = default;

  kind_type kind_ = kind_type::field;
  std::uint32_t no_ = 0;
  value key_;
  id_bytes element_{};
};

/// "/3/[\"name\"]/[-1]/<elm_...>": fields as numbers, keys in brackets, elements in angle brackets
/// (id text, or 32 hex digits for bytes that are not a valid id); "/" for the empty path.
[[nodiscard]] std::string to_string(const path &p);

} // namespace sl::fabric
