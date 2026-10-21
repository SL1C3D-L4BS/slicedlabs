#pragma once

// Runtime reflection (WO-087, SL-ARCH-003 ARCH3-R133 to R136, episode 096, ADR-0026, ADR-0027).
// type_registry describes each reflected type: a stable name and its FNV-1a-64 type key, size,
// alignment, fields (name, offset, size, scalar kind, flags transient / snapshot / hashed and an
// optional Fabric field number), methods (name, parameter kinds, a noexcept invoker returning
// sl::result) and an optional Fabric schema type. The registry freezes with the world
// (type_registry_frozen after). encode, decode and hash_into walk the selected fields in
// declaration order, little-endian, floats by canonical bit pattern (-0.0 as +0.0, one quiet NaN);
// raw object bytes and padding are never read. fabric_binding ties a document-bound type to one
// record type of a frozen sl::fabric::schema_registry (ADR-0027); a transient type (one that binds
// no field) carries no schema type. dump() is the deterministic inspector view (V06-5).
//
// Layer L4 (includes error.hpp and replay.hpp). It sits below ecs/entity.hpp, so the entity kind
// and method_arg use entity_bits {index, generation}; ecs converts.
//
// Thread safety (SL-ARCH-003 `thread_safety`): type_registry add and freeze are owner-thread (the
// thread that constructed the registry; wrong_thread otherwise), before freeze; find, field,
// method, dump, encode, decode, hash_into, invoke and fabric_binding are any-thread after freeze
// and reentrant (an object is written by one thread at a time).

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace sl::runtime {

/// `limits` max_fields_per_type and max_method_arity (ARCH3-R174).
inline constexpr std::size_t max_fields_per_type = 64;
inline constexpr std::size_t max_method_arity = 4;

/// The scalar kinds of a reflected field (ARCH3-R133).
enum class field_kind : std::uint8_t {
  boolean,
  i8,
  i16,
  i32,
  i64,
  u8,
  u16,
  u32,
  u64,
  f32,
  f64,
  entity,
  record_id,
  bytes_fixed,
};

/// "bool", "i8", ..., "entity", "record_id", "bytes"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(field_kind k) noexcept;

/// The byte size of a kind: 1 for boolean, the width for integers and floats, 8 for entity, 16
/// for record_id; 0 for bytes_fixed (its size is per field).
[[nodiscard]] constexpr std::uint32_t kind_size(field_kind k) noexcept {
  switch (k) {
  case field_kind::boolean:
  case field_kind::i8:
  case field_kind::u8:
    return 1;
  case field_kind::i16:
  case field_kind::u16:
    return 2;
  case field_kind::i32:
  case field_kind::u32:
  case field_kind::f32:
    return 4;
  case field_kind::i64:
  case field_kind::u64:
  case field_kind::f64:
  case field_kind::entity:
    return 8;
  case field_kind::record_id:
    return 16;
  case field_kind::bytes_fixed:
    return 0;
  }
  return 0;
}

/// Field flags (ARCH3-R133, `determinism`): transient fields are neither snapshotted nor hashed.
enum class field_flags : std::uint8_t { none = 0, transient = 1, snapshot = 2, hashed = 4 };

[[nodiscard]] constexpr field_flags operator|(field_flags a, field_flags b) noexcept {
  return static_cast<field_flags>(
      static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b)));
}
[[nodiscard]] constexpr field_flags operator&(field_flags a, field_flags b) noexcept {
  return static_cast<field_flags>(
      static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b)));
}
constexpr field_flags &operator|=(field_flags &a, field_flags b) noexcept { return a = a | b; }
constexpr field_flags &operator&=(field_flags &a, field_flags b) noexcept { return a = a & b; }
/// True when any flag is set.
[[nodiscard]] constexpr bool any(field_flags f) noexcept { return f != field_flags::none; }

/// The bits of an ecs::entity {index, generation} (L4 cannot name ecs::entity).
struct entity_bits {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  friend constexpr bool operator==(const entity_bits &, const entity_bits &) noexcept = default;
};

struct field_info {
  std::string_view name;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  field_kind kind = field_kind::boolean;
  field_flags flags = field_flags::none;
  std::uint32_t fabric_field = 0; // 0: not bound to a Fabric field
};

/// A method argument or result: bool, every signed integer as int64, every unsigned one as uint64,
/// both floats as double, an entity as entity_bits.
using method_arg = std::variant<bool, std::int64_t, std::uint64_t, double, entity_bits>;

/// A method invoker: noexcept by type (ARCH3-R010); the arguments are already checked.
using method_invoker = result<method_arg> (*)(void *obj, std::span<const method_arg> args) noexcept;

struct method_info {
  std::string_view name;
  std::span<const field_kind> params; // at most max_method_arity; no record_id or bytes_fixed
  method_invoker invoke = nullptr;
};

struct type_info {
  std::string_view name;
  std::uint32_t size = 0;
  std::uint32_t align = 0;
  std::vector<field_info> fields;   // declaration order (strictly ascending offsets)
  std::vector<method_info> methods; // registration order
  std::optional<std::string> schema_type;
  std::uint64_t type_key = 0; // FNV-1a-64 of name, set by type_registry::add
};

/// FNV-1a-64 of the bytes of `name` (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
[[nodiscard]] constexpr std::uint64_t type_key_of(std::string_view name) noexcept {
  std::uint64_t h = 0xcbf2'9ce4'8422'2325ULL;
  for (const char c : name) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 0x0000'0100'0000'01b3ULL;
  }
  return h;
}

namespace detail {

/// The kind of a member type; unsupported member types do not compile.
template <class M> struct member_kind;
template <> struct member_kind<bool> {
  static constexpr field_kind kind = field_kind::boolean;
};
template <> struct member_kind<std::int8_t> {
  static constexpr field_kind kind = field_kind::i8;
};
template <> struct member_kind<std::int16_t> {
  static constexpr field_kind kind = field_kind::i16;
};
template <> struct member_kind<std::int32_t> {
  static constexpr field_kind kind = field_kind::i32;
};
template <> struct member_kind<std::int64_t> {
  static constexpr field_kind kind = field_kind::i64;
};
template <> struct member_kind<std::uint8_t> {
  static constexpr field_kind kind = field_kind::u8;
};
template <> struct member_kind<std::uint16_t> {
  static constexpr field_kind kind = field_kind::u16;
};
template <> struct member_kind<std::uint32_t> {
  static constexpr field_kind kind = field_kind::u32;
};
template <> struct member_kind<std::uint64_t> {
  static constexpr field_kind kind = field_kind::u64;
};
template <> struct member_kind<float> {
  static constexpr field_kind kind = field_kind::f32;
};
template <> struct member_kind<double> {
  static constexpr field_kind kind = field_kind::f64;
};
template <> struct member_kind<entity_bits> {
  static constexpr field_kind kind = field_kind::entity;
};
template <> struct member_kind<fabric::record_id> {
  static constexpr field_kind kind = field_kind::record_id;
};
template <std::size_t N> struct member_kind<std::array<std::byte, N>> {
  static constexpr field_kind kind = field_kind::bytes_fixed;
};
template <std::size_t N> struct member_kind<std::array<std::uint8_t, N>> {
  static constexpr field_kind kind = field_kind::bytes_fixed;
};

} // namespace detail

/// Builds the type_info of a standard-layout T; the registry checks it in add().
template <class T> class type_builder {
  static_assert(std::is_standard_layout_v<T>, "reflected types must be standard-layout (offsetof)");
  static_assert(sizeof(T) <= 0xFFFF'FFFFU, "reflected types are below 4 GiB");

public:
  explicit type_builder(std::string_view name) noexcept {
    info_.name = name;
    info_.size = static_cast<std::uint32_t>(sizeof(T));
    info_.align = static_cast<std::uint32_t>(alignof(T));
    info_.type_key = type_key_of(name);
  }

  /// A field of a scalar kind (size from the kind; a bytes_fixed field goes through bytes()).
  type_builder &field(std::string_view name, std::uint32_t offset, field_kind kind,
                      field_flags flags, std::uint32_t fabric_field = 0) noexcept {
    info_.fields.push_back(field_info{.name = name,
                                      .offset = offset,
                                      .size = kind_size(kind),
                                      .kind = kind,
                                      .flags = flags,
                                      .fabric_field = fabric_field});
    return *this;
  }

  /// A bytes_fixed field of `size` bytes.
  type_builder &bytes(std::string_view name, std::uint32_t offset, std::uint32_t size,
                      field_flags flags, std::uint32_t fabric_field = 0) noexcept {
    info_.fields.push_back(field_info{.name = name,
                                      .offset = offset,
                                      .size = size,
                                      .kind = field_kind::bytes_fixed,
                                      .flags = flags,
                                      .fabric_field = fabric_field});
    return *this;
  }

  /// A field whose kind and size follow from its member type M (SL_RT_FIELD).
  template <class M>
  type_builder &field_of(std::string_view name, std::size_t offset, field_flags flags,
                         std::uint32_t fabric_field = 0) noexcept {
    static_assert(std::is_trivially_copyable_v<M>, "reflected fields are trivially copyable");
    static_assert(sizeof(M) <= 0xFFFF'FFFFU);
    return bytes_or_field(name, static_cast<std::uint32_t>(offset), detail::member_kind<M>::kind,
                          static_cast<std::uint32_t>(sizeof(M)), flags, fabric_field);
  }

  /// A method; `params` must outlive the registration only until add() (the registry copies).
  type_builder &method(std::string_view name, std::span<const field_kind> params,
                       method_invoker invoker) noexcept {
    info_.methods.push_back(method_info{.name = name, .params = params, .invoke = invoker});
    return *this;
  }

  /// The Fabric record type a document-bound type binds to (ARCH3-R135).
  type_builder &schema(std::string_view fabric_type) noexcept {
    info_.schema_type = std::string(fabric_type);
    return *this;
  }

  [[nodiscard]] const type_info &info() const noexcept { return info_; }
  /// A copy of the description (the builder stays usable).
  // cppcheck-suppress returnByReference ; build() hands out a copy on purpose, info() is the view
  [[nodiscard]] type_info build() const noexcept { return info_; }

private:
  type_builder &bytes_or_field(std::string_view name, std::uint32_t offset, field_kind kind,
                               std::uint32_t size, field_flags flags,
                               std::uint32_t fabric_field) noexcept {
    if (kind == field_kind::bytes_fixed) {
      return bytes(name, offset, size, flags, fabric_field);
    }
    return field(name, offset, kind, flags, fabric_field);
  }

  type_info info_;
};

/// `SL_RT_FIELD(b, T, member, flags)`: a field named after the member, at offsetof(T, member),
/// with the kind and size of its type. SL_RT_FIELD_BOUND also binds Fabric field number `no`.
#define SL_RT_FIELD(builder, T, member, flags)                                                     \
  (builder).template field_of<decltype(T::member)>(#member, offsetof(T, member), (flags))
#define SL_RT_FIELD_BOUND(builder, T, member, flags, no)                                           \
  (builder).template field_of<decltype(T::member)>(#member, offsetof(T, member), (flags), (no))

namespace detail {
/// The registry's owned copy of one type: names and parameter lists the views point into.
struct type_entry {
  type_info info;
  std::vector<std::string> names;
  std::vector<std::vector<field_kind>> params;
};
} // namespace detail

class type_registry {
public:
  /// The constructing thread is the owner thread of add and freeze.
  type_registry() noexcept;

  /// Checks and stores a copy of `info` (names and parameter lists included) and sets its type
  /// key: wrong_thread off the owner thread, type_registry_frozen after freeze, invalid_argument
  /// for a malformed type (more than max_fields_per_type fields, a method above max_method_arity,
  /// fields out of bounds, overlapping or out of declaration order, bad flags, schema rules),
  /// already_exists for a repeated name or type key. The pointer is stable for the registry's
  /// lifetime.
  [[nodiscard]] result<const type_info *> add(const type_info &info) noexcept;
  /// type_unknown for an unregistered name.
  [[nodiscard]] result<const type_info *> find(std::string_view name) const noexcept;
  /// field_unknown when `t` has no field `name` (reads only `t`, hence static).
  [[nodiscard]] static result<const field_info *> field(const type_info &t,
                                                        std::string_view name) noexcept;
  /// type_unknown when `t` has no method `name` (`error_codes`: type_unknown covers method lookup).
  [[nodiscard]] static result<const method_info *> method(const type_info &t,
                                                          std::string_view name) noexcept;
  /// Freezes the registry (owner thread; wrong_thread otherwise). Freezing twice succeeds.
  [[nodiscard]] result<void> freeze() noexcept;
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }

  /// One JSON object per type and line, types sorted by name (bytewise), byte-stable.
  [[nodiscard]] std::string dump() const noexcept;

private:
  std::map<std::string, detail::type_entry, std::less<>> types_;
  std::thread::id owner_;
  bool frozen_ = false;
};

/// Calls m.invoke after checking the arguments: reflect_mismatch for a wrong count or a kind the
/// argument does not hold or fit (integers by range, f32 by magnitude); invalid_argument for a
/// null object or invoker.
[[nodiscard]] result<method_arg> invoke(const method_info &m, void *obj,
                                        std::span<const method_arg> args) noexcept;

/// The byte count encode() appends for the fields `select` picks.
[[nodiscard]] std::size_t encoded_size(const type_info &t, field_flags select) noexcept;

/// Appends the fields whose flags intersect `select`, in declaration order: little-endian, bool as
/// one byte 0 or 1, floats by canonical bit pattern, entity as index then generation (two u32),
/// record_id and bytes_fixed as their raw bytes. Never reads padding.
void encode(const type_info &t, const void *obj, field_flags select,
            std::vector<std::byte> &out) noexcept;

/// The inverse of encode for the same `select`: invalid_argument for a wrong length, a bool byte
/// other than 0 or 1 or a null object; the object is unchanged on error.
[[nodiscard]] result<void> decode(const type_info &t, std::span<const std::byte> in,
                                  field_flags select, void *obj) noexcept;

/// Hashes the selected fields in declaration order through state_hasher (its tags and canonical
/// floats): entity as two u32, record_id and bytes_fixed as one bytes value.
void hash_into(const type_info &t, const void *obj, field_flags select,
               det::state_hasher &h) noexcept;

/// The binding of a document-bound type to one record type of a frozen schema_registry
/// (ARCH3-R135, ADR-0027).
class fabric_binding {
public:
  struct bound_field {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    field_kind kind = field_kind::boolean;
    std::uint32_t fabric_field = 0;
    bool required = true;
    std::optional<fabric::value> default_value;
  };

  /// invalid_argument when `t` has no schema type or `frozen` is not frozen;
  /// component_schema_mismatch when `fabric_type` is not t's schema type, is not a record struct
  /// type of `frozen`, or a field with a Fabric number is absent from it or has an incompatible
  /// kind (bool, iN and uN of the same width, f32, f64, id or ref for record_id; entity and
  /// bytes_fixed never bind).
  [[nodiscard]] static result<fabric_binding> bind(const type_info &t,
                                                   const fabric::schema_registry &frozen,
                                                   std::string_view fabric_type) noexcept;

  /// A struct value holding every bound field; invalid_argument for a NaN or infinite float.
  [[nodiscard]] result<fabric::value> to_value(const void *obj) const noexcept;
  /// Writes every bound field from a struct value (all or nothing): an absent field takes its
  /// Fabric default, an absent optional field without one is zero, an absent required field, a
  /// wrong value kind or an out-of-range integer is invalid_argument.
  [[nodiscard]] result<void> from_value(const fabric::value &v, void *obj) const noexcept;
  /// The path of a bound field ({field(no)}); empty when `fabric_field` is not bound.
  [[nodiscard]] fabric::path path_of(std::uint32_t fabric_field) const noexcept;

  [[nodiscard]] const type_info *type() const noexcept { return type_; }
  [[nodiscard]] const fabric::type_id &fabric_type() const noexcept { return fabric_type_; }
  [[nodiscard]] std::span<const bound_field> fields() const noexcept { return fields_; }

private:
  const type_info *type_ = nullptr;
  fabric::type_id fabric_type_;
  std::vector<bound_field> fields_;
};

} // namespace sl::runtime
