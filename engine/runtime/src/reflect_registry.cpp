// type_registry (WO-087, SL-ARCH-003 ARCH3-R133, R135, R136, `limits` max_fields_per_type and
// max_method_arity, `immutability` type registry): add checks one type and stores an owned copy
// whose views point into the entry, so a registered type never depends on the caller's strings;
// dump writes one JSON object per type in name order with integers through std::to_chars, so the
// bytes do not depend on the locale.

#include "sl/runtime/reflect.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

constexpr field_flags all_flags =
    field_flags::transient | field_flags::snapshot | field_flags::hashed;

bool method_param_ok(field_kind k) noexcept { return k < field_kind::record_id; }

/// invalid_argument text for the first malformed field, or empty.
std::string_view check_fields(const type_info &t) noexcept {
  std::uint64_t end = 0;
  std::set<std::string_view> names;
  std::set<std::uint32_t> numbers;
  for (const field_info &f : t.fields) {
    if (f.name.empty()) {
      return "a field has an empty name";
    }
    if (!names.insert(f.name).second) {
      return "a field name repeats";
    }
    if (f.kind > field_kind::bytes_fixed) {
      return "a field has an unknown kind";
    }
    const std::uint32_t want = kind_size(f.kind);
    if ((want != 0 && f.size != want) || f.size == 0) {
      return "a field size does not match its kind";
    }
    if (f.offset < end) {
      return "fields overlap or are not in declaration order";
    }
    end = std::uint64_t{f.offset} + f.size;
    if (end > t.size) {
      return "a field lies outside the type";
    }
    if ((f.flags & all_flags) != f.flags) {
      return "a field has an unknown flag";
    }
    const bool transient = any(f.flags & field_flags::transient);
    if (transient && any(f.flags & (field_flags::snapshot | field_flags::hashed))) {
      return "a transient field cannot be snapshotted or hashed";
    }
    if (f.fabric_field != 0) {
      if (transient) {
        return "a transient field cannot be bound to a Fabric field";
      }
      if (!numbers.insert(f.fabric_field).second) {
        return "a Fabric field number repeats";
      }
    }
  }
  const bool has_schema = t.schema_type.has_value();
  if (has_schema && t.schema_type->empty()) {
    return "the schema type is empty";
  }
  if (!has_schema && !numbers.empty()) {
    return "a type without a schema type binds a Fabric field";
  }
  if (has_schema && numbers.empty()) {
    return "a transient type (no bound field) carries a schema type";
  }
  return {};
}

std::string_view check_methods(const type_info &t) noexcept {
  std::set<std::string_view> names;
  for (const method_info &m : t.methods) {
    if (m.name.empty()) {
      return "a method has an empty name";
    }
    if (!names.insert(m.name).second) {
      return "a method name repeats";
    }
    if (m.invoke == nullptr) {
      return "a method has no invoker";
    }
    if (m.params.size() > max_method_arity) {
      return "a method takes more than max_method_arity arguments";
    }
    if (!std::ranges::all_of(m.params, method_param_ok)) {
      return "a method parameter kind is not a method_arg kind";
    }
  }
  return {};
}

std::string_view check_type(const type_info &t) noexcept {
  if (t.name.empty()) {
    return "the type name is empty";
  }
  if (t.size == 0 || t.align == 0 || !std::has_single_bit(t.align) || t.size % t.align != 0) {
    return "size and alignment are not a valid object layout";
  }
  if (t.fields.size() > max_fields_per_type) {
    return "more than max_fields_per_type fields";
  }
  if (const std::string_view why = check_fields(t); !why.empty()) {
    return why;
  }
  return check_methods(t);
}

void append_uint(std::string &out, std::uint64_t v) noexcept {
  std::array<char, 24> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  if (ec == std::errc{}) {
    out.append(buf.data(), end);
  }
}

void append_hex64(std::string &out, std::uint64_t v) noexcept {
  constexpr std::string_view digits = "0123456789abcdef";
  out += "0x";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out += digits[static_cast<std::size_t>((v >> static_cast<unsigned>(shift)) & 0xFU)];
  }
}

/// A JSON string: quote, backslash and control bytes escaped; other bytes as they are.
void append_string(std::string &out, std::string_view s) noexcept {
  constexpr std::string_view digits = "0123456789abcdef";
  out += '"';
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += R"(\n)";
    } else if (c == '\t') {
      out += R"(\t)";
    } else if (c == '\r') {
      out += R"(\r)";
    } else if (u < 0x20U) {
      out += "\\u00";
      out += digits[u >> 4U];
      out += digits[u & 0xFU];
    } else {
      out += c;
    }
  }
  out += '"';
}

void append_flags(std::string &out, field_flags f) noexcept {
  out += '[';
  bool first = true;
  const std::array<std::pair<field_flags, std::string_view>, 3> names{{
      {field_flags::transient, "transient"},
      {field_flags::snapshot, "snapshot"},
      {field_flags::hashed, "hashed"},
  }};
  for (const auto &[flag, name] : names) {
    if (any(f & flag)) {
      if (!first) {
        out += ',';
      }
      first = false;
      append_string(out, name);
    }
  }
  out += ']';
}

void append_type(std::string &out, const type_info &t) noexcept {
  out += R"({"name":)";
  append_string(out, t.name);
  out += R"(,"type_key":")";
  append_hex64(out, t.type_key);
  out += R"(","size":)";
  append_uint(out, t.size);
  out += R"(,"align":)";
  append_uint(out, t.align);
  out += R"(,"schema_type":)";
  if (t.schema_type) {
    append_string(out, *t.schema_type);
  } else {
    out += "null";
  }
  out += R"(,"fields":[)";
  for (std::size_t i = 0; i < t.fields.size(); ++i) {
    const field_info &f = t.fields[i];
    out += i == 0 ? R"({"name":)" : R"(,{"name":)";
    append_string(out, f.name);
    out += R"(,"offset":)";
    append_uint(out, f.offset);
    out += R"(,"size":)";
    append_uint(out, f.size);
    out += R"(,"kind":)";
    append_string(out, to_string(f.kind));
    out += R"(,"flags":)";
    append_flags(out, f.flags);
    out += R"(,"fabric_field":)";
    append_uint(out, f.fabric_field);
    out += '}';
  }
  out += R"(],"methods":[)";
  for (std::size_t i = 0; i < t.methods.size(); ++i) {
    const method_info &m = t.methods[i];
    out += i == 0 ? R"({"name":)" : R"(,{"name":)";
    append_string(out, m.name);
    out += R"(,"params":[)";
    for (std::size_t p = 0; p < m.params.size(); ++p) {
      if (p != 0) {
        out += ',';
      }
      append_string(out, to_string(m.params[p]));
    }
    out += "]}";
  }
  out += "]}\n";
}

} // namespace

std::string_view to_string(field_kind k) noexcept {
  switch (k) {
  case field_kind::boolean:
    return "bool";
  case field_kind::i8:
    return "i8";
  case field_kind::i16:
    return "i16";
  case field_kind::i32:
    return "i32";
  case field_kind::i64:
    return "i64";
  case field_kind::u8:
    return "u8";
  case field_kind::u16:
    return "u16";
  case field_kind::u32:
    return "u32";
  case field_kind::u64:
    return "u64";
  case field_kind::f32:
    return "f32";
  case field_kind::f64:
    return "f64";
  case field_kind::entity:
    return "entity";
  case field_kind::record_id:
    return "record_id";
  case field_kind::bytes_fixed:
    return "bytes";
  }
  return "unknown";
}

type_registry::type_registry() noexcept : owner_(std::this_thread::get_id()) {}

result<const type_info *> type_registry::add(const type_info &info) noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "type_registry::add off the owner thread");
  }
  if (frozen_) {
    return make_error(errc::type_registry_frozen, "type_registry::add after freeze");
  }
  if (const std::string_view why = check_type(info); !why.empty()) {
    return make_error(errc::invalid_argument,
                      "type_registry::add: " + std::string(info.name) + ": " + std::string(why));
  }
  if (types_.contains(info.name)) {
    return make_error(errc::already_exists,
                      "type_registry::add: the type " + std::string(info.name) + " exists");
  }
  const std::uint64_t key = type_key_of(info.name);
  for (const auto &[name, entry] : types_) {
    if (entry.info.type_key == key) {
      return make_error(errc::already_exists, "type_registry::add: the type key of " +
                                                  std::string(info.name) + " equals that of " +
                                                  name);
    }
  }
  auto [it, inserted] = types_.try_emplace(std::string(info.name));
  detail::type_entry &e = it->second;
  e.names.reserve(info.fields.size() + info.methods.size());
  e.params.reserve(info.methods.size());
  e.info = info;
  e.info.name = it->first;
  e.info.type_key = key;
  for (field_info &f : e.info.fields) {
    f.name = e.names.emplace_back(f.name);
  }
  for (method_info &m : e.info.methods) {
    m.name = e.names.emplace_back(m.name);
    m.params = e.params.emplace_back(m.params.begin(), m.params.end());
  }
  return &e.info;
}

result<const type_info *> type_registry::find(std::string_view name) const noexcept {
  const auto it = types_.find(name);
  if (it == types_.end()) {
    return make_error(errc::type_unknown, "type_registry::find: no type " + std::string(name));
  }
  return &it->second.info;
}

result<const field_info *> type_registry::field(const type_info &t,
                                                std::string_view name) noexcept {
  if (const auto it = std::ranges::find(t.fields, name, &field_info::name); it != t.fields.end()) {
    return &*it;
  }
  return make_error(errc::field_unknown, "type_registry::field: " + std::string(t.name) +
                                             " has no field " + std::string(name));
}

result<const method_info *> type_registry::method(const type_info &t,
                                                  std::string_view name) noexcept {
  if (const auto it = std::ranges::find(t.methods, name, &method_info::name);
      it != t.methods.end()) {
    return &*it;
  }
  return make_error(errc::type_unknown, "type_registry::method: " + std::string(t.name) +
                                            " has no method " + std::string(name));
}

result<void> type_registry::freeze() noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "type_registry::freeze off the owner thread");
  }
  frozen_ = true;
  return {};
}

std::string type_registry::dump() const noexcept {
  std::string out;
  for (const auto &[name, entry] : types_) {
    append_type(out, entry.info);
  }
  return out;
}

} // namespace sl::runtime
