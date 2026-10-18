#pragma once

// Type descriptors (WO-039, SL-ARCH-001 ARCH1-R12 to ARCH1-R18, ADR-0013).
//
// A type_desc describes one type of a kind in SL-ARCH-001 `type_kinds`. Types refer to each other
// by type id, which is derive(type, well_known(namespace, 1), qualified name) (ARCH1-R12); the
// schema registry (schema.hpp) resolves the references and freezes the set. The scalar kinds,
// ref, blob_ref and external_ref exist once as built-in types named after their kind ("u32",
// "ref", "external_ref"), registered by every schema registry, so a field of type u32 names
// type_id_of("u32").
//
// Shapes by kind:
//   struct_       fields, reserved (removed field numbers, never reused: ARCH1-R13)
//   enum_         enumerators (stable numbers; the value is a uint)
//   variant       cases (stable numbers, each with a type)
//   list, oset    element
//   map           key (a string, integer or id type) and element (the value type)
//   tagged        element (the base type: meters is tagged f64)
//   ref           targets and strength sit on the field that holds the ref (field_desc::ref) and
//                 apply to every ref reached from the field through tagged, list, map and oset
//                 (ARCH1-R18)
//   external_ref  the built-in struct {1 system string, 2 locator string, 3 content digest
//                 optional}
// A record type (ARCH1-R17) is one with a record class, a state class or the document_root flag;
// freeze() requires all of: a domain, an assigned record class and a state class in {document,
// derived, observation}.

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// The kinds of SL-ARCH-001 `type_kinds`; numbers are stable.
enum class type_kind : std::uint8_t {
  bool_ = 1,
  i8 = 2,
  i16 = 3,
  i32 = 4,
  i64 = 5,
  u8 = 6,
  u16 = 7,
  u32 = 8,
  u64 = 9,
  f32 = 10,
  f64 = 11,
  string = 12,
  bytes = 13,
  id = 14,
  digest = 15,
  enum_ = 16,
  struct_ = 17,
  list = 18,
  map = 19,
  oset = 20,
  variant = 21,
  tagged = 22,
  ref = 23,
  blob_ref = 24,
  external_ref = 25,
};

/// "bool", "i8", ..., "enum", "struct", ..., "external_ref"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(type_kind k) noexcept;
[[nodiscard]] std::optional<type_kind> type_kind_from_string(std::string_view name) noexcept;

/// True for i8..i64 and u8..u64.
[[nodiscard]] constexpr bool is_integer(type_kind k) noexcept {
  return k >= type_kind::i8 && k <= type_kind::u64;
}

/// The kinds that are built-in types and carry no shape of their own.
[[nodiscard]] constexpr bool is_builtin_kind(type_kind k) noexcept {
  return (k >= type_kind::bool_ && k <= type_kind::digest) || k == type_kind::ref ||
         k == type_kind::blob_ref || k == type_kind::external_ref;
}

enum class ref_strength : std::uint8_t {
  weak = 1,   // no dependency edge, may dangle
  strong = 2, // edge, must be live at commit
  owned = 3,  // containment edge, cascades on delete
};

enum class presence : std::uint8_t { required = 1, optional = 2 };

/// SL-ARCH-001 `state_classes`; none means "not a record type" (and is refused on one).
enum class state_class : std::uint8_t { none = 0, document = 1, derived = 2, observation = 3 };

[[nodiscard]] std::string_view to_string(ref_strength s) noexcept;
[[nodiscard]] std::string_view to_string(presence p) noexcept;
[[nodiscard]] std::string_view to_string(state_class s) noexcept;

struct type_version {
  std::uint32_t major = 1;
  std::uint32_t minor = 0;

  friend constexpr auto operator<=>(const type_version &, const type_version &) noexcept = default;
};

/// The targets and strength of the refs a field holds (ARCH1-R18).
struct ref_spec {
  ref_strength strength = ref_strength::weak;
  std::vector<type_id> targets;

  friend bool operator==(const ref_spec &, const ref_spec &) = default;
};

struct field_desc {
  std::uint32_t no = 0; // stable, >= 1 (ARCH1-R13)
  std::string name;     // a label
  type_id type;
  fabric::presence presence = fabric::presence::required;
  std::optional<value> default_value; // optional fields only; applied by readers (ARCH1-R14)
  std::uint32_t since_minor = 0;
  std::optional<std::uint32_t> deprecated_in;
  std::optional<ref_spec> ref;
  std::string doc;

  friend bool operator==(const field_desc &, const field_desc &) = default;
};

struct enumerator_desc {
  std::uint32_t no = 0;
  std::string name;
  std::string doc;

  friend bool operator==(const enumerator_desc &, const enumerator_desc &) = default;
};

struct case_desc {
  std::uint32_t no = 0;
  std::string name;
  type_id type;
  std::string doc;

  friend bool operator==(const case_desc &, const case_desc &) = default;
};

struct type_desc {
  type_id id;       // type_id_of(name)
  std::string name; // qualified: dot-separated identifiers ("content.scene.Entity")
  type_kind kind = type_kind::struct_;
  type_version version;
  std::string domain;
  id_class record_class = id_class::none;
  fabric::state_class state_class = fabric::state_class::none;
  bool document_root = false;
  std::vector<field_desc> fields;
  std::vector<std::uint32_t> reserved;
  std::vector<enumerator_desc> enumerators;
  std::vector<case_desc> cases;
  std::optional<type_id> element;
  std::optional<type_id> key;
  std::string doc;

  /// A record type: a record class, a state class or the document_root flag is set.
  [[nodiscard]] bool is_record() const noexcept {
    return record_class != id_class::none || state_class != fabric::state_class::none ||
           document_root;
  }
  [[nodiscard]] const field_desc *find_field(std::uint32_t no) const noexcept;
  [[nodiscard]] const enumerator_desc *find_enumerator(std::uint32_t no) const noexcept;
  [[nodiscard]] const case_desc *find_case(std::uint32_t no) const noexcept;

  friend bool operator==(const type_desc &, const type_desc &) = default;
};

/// derive(type, well_known(namespace, 1), qualified_name) (ARCH1-R12).
[[nodiscard]] type_id type_id_of(std::string_view qualified_name) noexcept;

/// The built-in types, in kind order: bool, i8..u64, f32, f64, string, bytes, id, digest, ref,
/// blob_ref, external_ref, all in domain "fabric", version 1.0.
[[nodiscard]] std::span<const type_desc> builtin_types();
[[nodiscard]] bool is_builtin_type(const type_id &id) noexcept;

/// The rules a type must meet on its own (FAB-INV-TY-01; freeze adds the cross-type rules):
/// `schema_invalid` naming the first broken rule in this order:
///   R-TY-NAME      qualified name of dot-separated identifiers; field, enumerator, case names
///                  are identifiers
///   R-TY-ID        id is type_id_of(name)
///   R-TY-VERSION   major >= 1
///   R-TY-SHAPE     only the parts of the kind's shape are set (fields and reserved for struct,
///                  enumerators for enum, cases for variant, element for list, oset, map and
///                  tagged, key for map); a built-in kind is not declared by a user type
///   R-TY-FIELD     field numbers >= 1 and unique; field names unique; each field has a type
///   R-TY-RESERVED  reserved numbers unique and used by no field
///   R-TY-DEFAULT   a default only on an optional field
///   R-TY-SINCE     since_minor <= version.minor; deprecated_in within [since_minor, minor]
///   R-TY-REF       a ref spec has at least one target and no target twice
///   R-TY-ENUM      at least one enumerator; numbers and names unique
///   R-TY-CASE      at least one case; numbers and names unique; each case has a type
[[nodiscard]] result<void> check_well_formed(const type_desc &t);

/// A valid fractional order key (FAB-INV-TY-06): 1 to 2^24 bytes, the last byte non-zero, so a
/// key strictly between any two keys exists (keys order by bytes, a prefix first).
[[nodiscard]] bool is_valid_order_key(byte_span key) noexcept;

/// A valid order key strictly between `lo` and `hi`; an empty bound is open. `invalid_argument`
/// unless each non-empty bound is valid and lo < hi.
[[nodiscard]] result<std::vector<std::uint8_t>> order_key_between(byte_span lo, byte_span hi);

/// Reading with defaults (ARCH1-R14): the field when the struct holds it, else the field's default,
/// else nullopt. The struct is not changed: the default is never stored.
[[nodiscard]] std::optional<value> read_field(const value &s, const field_desc &f);

} // namespace sl::fabric
