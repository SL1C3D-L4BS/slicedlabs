// fabric_binding (WO-087, SL-ARCH-003 ARCH3-R135, ADR-0027, ARCH1-R62): binds a document-bound
// reflected type to one record struct type of a frozen schema_registry through the public Fabric
// schema API only (schema.hpp, type.hpp, value.hpp). A field kind binds to the Fabric kind of the
// same family and width, resolved through tagged bases; record_id binds to id or ref. to_value
// and from_value convert between the bound fields and a Fabric struct value; Fabric errors are
// reported as runtime codes (invalid_argument), never passed through.

#include "sl/runtime/reflect.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

/// A tagged chain longer than this is not resolved (the frozen registry has no tagged cycle).
constexpr std::size_t max_tagged_depth = 64;

std::optional<fabric::type_kind> resolved_kind(const fabric::schema_registry &reg,
                                               const fabric::type_id &id) noexcept {
  const fabric::type_desc *d = reg.find(id);
  for (std::size_t i = 0; d != nullptr && i < max_tagged_depth; ++i) {
    if (d->kind != fabric::type_kind::tagged) {
      return d->kind;
    }
    if (!d->element) {
      return std::nullopt;
    }
    d = reg.find(*d->element);
  }
  return std::nullopt;
}

bool compatible(field_kind k, fabric::type_kind fk) noexcept {
  using fabric::type_kind;
  switch (k) {
  case field_kind::boolean:
    return fk == type_kind::bool_;
  case field_kind::i8:
    return fk == type_kind::i8;
  case field_kind::i16:
    return fk == type_kind::i16;
  case field_kind::i32:
    return fk == type_kind::i32;
  case field_kind::i64:
    return fk == type_kind::i64;
  case field_kind::u8:
    return fk == type_kind::u8;
  case field_kind::u16:
    return fk == type_kind::u16;
  case field_kind::u32:
    return fk == type_kind::u32;
  case field_kind::u64:
    return fk == type_kind::u64;
  case field_kind::f32:
    return fk == type_kind::f32;
  case field_kind::f64:
    return fk == type_kind::f64;
  case field_kind::record_id:
    return fk == type_kind::id || fk == type_kind::ref;
  case field_kind::entity:
  case field_kind::bytes_fixed:
    return false;
  }
  return false;
}

const std::byte *at(const void *obj, std::uint32_t offset) noexcept {
  return static_cast<const std::byte *>(obj) + offset;
}

std::byte *at_mut(void *obj, std::uint32_t offset) noexcept {
  return static_cast<std::byte *>(obj) + offset;
}

template <class U> U load(const void *obj, std::uint32_t offset) noexcept {
  U v{};
  std::memcpy(&v, at(obj, offset), sizeof(U));
  return v;
}

/// The bytes a field holds, in the object's representation.
using staged_bytes = std::array<std::byte, 16>;

template <class U> staged_bytes stage(U v) noexcept {
  staged_bytes out{};
  std::memcpy(out.data(), &v, sizeof(U));
  return out;
}

template <class U> bool in_range(std::int64_t v) noexcept {
  return v >= std::numeric_limits<U>::min() && v <= std::numeric_limits<U>::max();
}

template <class U> bool in_range_u(std::uint64_t v) noexcept {
  return v <= std::numeric_limits<U>::max();
}

result<fabric::value> float_value(result<fabric::value> v) noexcept {
  if (!v) {
    return make_error(errc::invalid_argument,
                      "fabric_binding::to_value: a float field is NaN or infinite");
  }
  return std::move(*v);
}

result<fabric::value> field_value(const fabric_binding::bound_field &b, const void *obj) noexcept {
  switch (b.kind) {
  case field_kind::boolean:
    return fabric::value::boolean(load<std::uint8_t>(obj, b.offset) != 0);
  case field_kind::i8:
    return fabric::value::integer(load<std::int8_t>(obj, b.offset));
  case field_kind::i16:
    return fabric::value::integer(load<std::int16_t>(obj, b.offset));
  case field_kind::i32:
    return fabric::value::integer(load<std::int32_t>(obj, b.offset));
  case field_kind::i64:
    return fabric::value::integer(load<std::int64_t>(obj, b.offset));
  case field_kind::u8:
    return fabric::value::uint(load<std::uint8_t>(obj, b.offset));
  case field_kind::u16:
    return fabric::value::uint(load<std::uint16_t>(obj, b.offset));
  case field_kind::u32:
    return fabric::value::uint(load<std::uint32_t>(obj, b.offset));
  case field_kind::u64:
    return fabric::value::uint(load<std::uint64_t>(obj, b.offset));
  case field_kind::f32:
    return float_value(fabric::value::f32(load<float>(obj, b.offset)));
  case field_kind::f64:
    return float_value(fabric::value::f64(load<double>(obj, b.offset)));
  case field_kind::record_id:
    return fabric::value::id(load<fabric::id_bytes>(obj, b.offset));
  case field_kind::entity:
  case field_kind::bytes_fixed:
    break;
  }
  return make_error(errc::internal, "fabric_binding::to_value: an unbindable kind is bound");
}

result<staged_bytes> mismatch() noexcept {
  return make_error(errc::invalid_argument,
                    "fabric_binding::from_value: a field value does not fit its kind");
}

result<staged_bytes> signed_bytes(field_kind k, const fabric::value &v) noexcept {
  if (v.kind() != fabric::value_kind::uint && v.kind() != fabric::value_kind::nint) {
    return mismatch();
  }
  const std::optional<std::int64_t> i = v.as_i64();
  if (!i) {
    return mismatch();
  }
  switch (k) {
  case field_kind::i8:
    return in_range<std::int8_t>(*i) ? stage(static_cast<std::int8_t>(*i)) : mismatch();
  case field_kind::i16:
    return in_range<std::int16_t>(*i) ? stage(static_cast<std::int16_t>(*i)) : mismatch();
  case field_kind::i32:
    return in_range<std::int32_t>(*i) ? stage(static_cast<std::int32_t>(*i)) : mismatch();
  default:
    return stage(*i);
  }
}

result<staged_bytes> unsigned_bytes(field_kind k, const fabric::value &v) noexcept {
  if (v.kind() != fabric::value_kind::uint) {
    return mismatch();
  }
  const std::uint64_t u = v.as_uint();
  switch (k) {
  case field_kind::u8:
    return in_range_u<std::uint8_t>(u) ? stage(static_cast<std::uint8_t>(u)) : mismatch();
  case field_kind::u16:
    return in_range_u<std::uint16_t>(u) ? stage(static_cast<std::uint16_t>(u)) : mismatch();
  case field_kind::u32:
    return in_range_u<std::uint32_t>(u) ? stage(static_cast<std::uint32_t>(u)) : mismatch();
  default:
    return stage(u);
  }
}

result<staged_bytes> field_bytes(field_kind k, const fabric::value &v) noexcept {
  switch (k) {
  case field_kind::boolean:
    return v.kind() == fabric::value_kind::bool_ ? stage(v.as_bool()) : mismatch();
  case field_kind::i8:
  case field_kind::i16:
  case field_kind::i32:
  case field_kind::i64:
    return signed_bytes(k, v);
  case field_kind::u8:
  case field_kind::u16:
  case field_kind::u32:
  case field_kind::u64:
    return unsigned_bytes(k, v);
  case field_kind::f32:
    return v.kind() == fabric::value_kind::f32 ? stage(v.as_f32()) : mismatch();
  case field_kind::f64:
    return v.kind() == fabric::value_kind::f64 ? stage(v.as_f64()) : mismatch();
  case field_kind::record_id:
    return v.kind() == fabric::value_kind::id ? stage(v.as_id_bytes()) : mismatch();
  case field_kind::entity:
  case field_kind::bytes_fixed:
    break;
  }
  return mismatch();
}

} // namespace

result<fabric_binding> fabric_binding::bind(const type_info &t,
                                            const fabric::schema_registry &frozen,
                                            std::string_view fabric_type) noexcept {
  if (!t.schema_type) {
    return make_error(errc::invalid_argument,
                      "fabric_binding::bind: " + std::string(t.name) +
                          " has no schema type (a transient type cannot bind)");
  }
  if (!frozen.frozen()) {
    return make_error(errc::invalid_argument, "fabric_binding::bind: the schema registry is not "
                                              "frozen");
  }
  if (*t.schema_type != fabric_type) {
    return make_error(errc::component_schema_mismatch,
                      "fabric_binding::bind: " + std::string(t.name) + " has the schema type " +
                          *t.schema_type + ", not " + std::string(fabric_type));
  }
  const fabric::type_desc *desc = frozen.find(fabric_type);
  if (desc == nullptr || desc->kind != fabric::type_kind::struct_ || !desc->is_record()) {
    return make_error(errc::component_schema_mismatch,
                      "fabric_binding::bind: " + std::string(fabric_type) +
                          " is not a record struct type of the schema registry");
  }
  fabric_binding b;
  b.type_ = &t;
  b.fabric_type_ = desc->id;
  for (const field_info &f : t.fields) {
    if (f.fabric_field == 0) {
      continue;
    }
    const fabric::field_desc *fd = desc->find_field(f.fabric_field);
    if (fd == nullptr) {
      return make_error(errc::component_schema_mismatch,
                        "fabric_binding::bind: " + std::string(fabric_type) + " has no field " +
                            std::to_string(f.fabric_field) + " for " + std::string(f.name));
    }
    const std::optional<fabric::type_kind> fk = resolved_kind(frozen, fd->type);
    if (!fk || !compatible(f.kind, *fk)) {
      return make_error(errc::component_schema_mismatch,
                        "fabric_binding::bind: the kind of " + std::string(f.name) +
                            " does not match Fabric field " + std::to_string(f.fabric_field));
    }
    b.fields_.push_back(bound_field{.offset = f.offset,
                                    .size = f.size,
                                    .kind = f.kind,
                                    .fabric_field = f.fabric_field,
                                    .required = fd->presence == fabric::presence::required,
                                    .default_value = fd->default_value});
  }
  return b;
}

result<fabric::value> fabric_binding::to_value(const void *obj) const noexcept {
  if (obj == nullptr) {
    return make_error(errc::invalid_argument, "fabric_binding::to_value: null object");
  }
  std::vector<fabric::field> out;
  out.reserve(fields_.size());
  for (const bound_field &b : fields_) {
    result<fabric::value> v = field_value(b, obj);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    out.push_back(fabric::field{.no = b.fabric_field, .v = std::move(*v)});
  }
  result<fabric::value> s = fabric::value::struct_(std::move(out));
  if (!s) {
    return make_error(errc::invalid_argument, "fabric_binding::to_value: the struct is invalid");
  }
  return std::move(*s);
}

result<void> fabric_binding::from_value(const fabric::value &v, void *obj) const noexcept {
  if (obj == nullptr) {
    return make_error(errc::invalid_argument, "fabric_binding::from_value: null object");
  }
  if (v.kind() != fabric::value_kind::struct_) {
    return make_error(errc::invalid_argument, "fabric_binding::from_value: not a struct value");
  }
  std::vector<staged_bytes> staged(fields_.size(), staged_bytes{});
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    const bound_field &b = fields_[i];
    const fabric::value *fv = v.find_field(b.fabric_field);
    if (fv == nullptr && b.default_value) {
      fv = &*b.default_value;
    }
    if (fv == nullptr) {
      if (b.required) {
        return make_error(errc::invalid_argument, "fabric_binding::from_value: required field " +
                                                      std::to_string(b.fabric_field) +
                                                      " is absent");
      }
      continue; // an absent optional field without a default is zero
    }
    result<staged_bytes> bytes = field_bytes(b.kind, *fv);
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    staged[i] = *bytes;
  }
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    std::memcpy(at_mut(obj, fields_[i].offset), staged[i].data(), fields_[i].size);
  }
  return {};
}

fabric::path fabric_binding::path_of(std::uint32_t fabric_field) const noexcept {
  if (std::ranges::any_of(fields_, [fabric_field](const bound_field &b) noexcept {
        return b.fabric_field == fabric_field;
      })) {
    return fabric::path{fabric::path_segment::field(fabric_field)};
  }
  return {};
}

} // namespace sl::runtime
