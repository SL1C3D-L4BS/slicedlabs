// Type descriptors (WO-039, SL-ARCH-001 ARCH1-R12 to ARCH1-R18). See type.hpp.

#include "sl/fabric/type.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <set>
#include <utility>

namespace sl::fabric {

namespace {

constexpr std::array<std::pair<type_kind, std::string_view>, 25> kind_names = {{
    {type_kind::bool_, "bool"},
    {type_kind::i8, "i8"},
    {type_kind::i16, "i16"},
    {type_kind::i32, "i32"},
    {type_kind::i64, "i64"},
    {type_kind::u8, "u8"},
    {type_kind::u16, "u16"},
    {type_kind::u32, "u32"},
    {type_kind::u64, "u64"},
    {type_kind::f32, "f32"},
    {type_kind::f64, "f64"},
    {type_kind::string, "string"},
    {type_kind::bytes, "bytes"},
    {type_kind::id, "id"},
    {type_kind::digest, "digest"},
    {type_kind::enum_, "enum"},
    {type_kind::struct_, "struct"},
    {type_kind::list, "list"},
    {type_kind::map, "map"},
    {type_kind::oset, "oset"},
    {type_kind::variant, "variant"},
    {type_kind::tagged, "tagged"},
    {type_kind::ref, "ref"},
    {type_kind::blob_ref, "blob_ref"},
    {type_kind::external_ref, "external_ref"},
}};

std::unexpected<error> invalid(const type_desc &t, std::string_view rule, std::string_view why) {
  return make_error(errc::schema_invalid,
                    std::format("{}: type '{}': {}", rule, t.name.empty() ? "?" : t.name, why));
}

bool is_identifier(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  const auto alpha = [](char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (!alpha(s.front())) {
    return false;
  }
  return std::ranges::all_of(s, [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

bool is_qualified_name(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  while (true) {
    const std::size_t dot = s.find('.');
    if (!is_identifier(s.substr(0, dot))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    s.remove_prefix(dot + 1);
  }
}

template <class Range, class Proj> bool has_duplicates(const Range &r, Proj proj) {
  using key = std::remove_cvref_t<std::invoke_result_t<Proj, decltype(*std::ranges::begin(r))>>;
  std::set<key> seen;
  for (const auto &x : r) {
    if (!seen.insert(std::invoke(proj, x)).second) {
      return true;
    }
  }
  return false;
}

type_desc builtin(type_kind k) {
  type_desc t;
  t.name = std::string(to_string(k));
  t.id = type_id_of(t.name);
  t.kind = k;
  t.domain = "fabric";
  return t;
}

std::vector<type_desc> make_builtins() {
  std::vector<type_desc> out;
  for (const auto &[k, name] : kind_names) {
    if (is_builtin_kind(k)) {
      out.push_back(builtin(k));
    }
  }
  // external_ref is the struct {1 system string, 2 locator string, 3 content digest optional}.
  const auto make_field = [](std::uint32_t no, std::string_view name, std::string_view type,
                             presence p) {
    field_desc f;
    f.no = no;
    f.name = std::string(name);
    f.type = type_id_of(type);
    f.presence = p;
    return f;
  };
  type_desc &ext = out.back();
  ext.fields = {make_field(1, "system", "string", presence::required),
                make_field(2, "locator", "string", presence::required),
                make_field(3, "content", "digest", presence::optional)};
  return out;
}

} // namespace

std::string_view to_string(type_kind k) noexcept {
  for (const auto &[kind, name] : kind_names) {
    if (kind == k) {
      return name;
    }
  }
  return "unknown";
}

std::optional<type_kind> type_kind_from_string(std::string_view name) noexcept {
  for (const auto &[kind, n] : kind_names) {
    if (n == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string_view to_string(ref_strength s) noexcept {
  switch (s) {
  case ref_strength::weak:
    return "weak";
  case ref_strength::strong:
    return "strong";
  case ref_strength::owned:
    return "owned";
  }
  return "unknown";
}

std::string_view to_string(presence p) noexcept {
  switch (p) {
  case presence::required:
    return "required";
  case presence::optional:
    return "optional";
  }
  return "unknown";
}

std::string_view to_string(state_class s) noexcept {
  switch (s) {
  case state_class::none:
    return "none";
  case state_class::document:
    return "document";
  case state_class::derived:
    return "derived";
  case state_class::observation:
    return "observation";
  }
  return "unknown";
}

const field_desc *type_desc::find_field(std::uint32_t no) const noexcept {
  const auto it = std::ranges::find(fields, no, &field_desc::no);
  return it == fields.end() ? nullptr : &*it;
}

const enumerator_desc *type_desc::find_enumerator(std::uint32_t no) const noexcept {
  const auto it = std::ranges::find(enumerators, no, &enumerator_desc::no);
  return it == enumerators.end() ? nullptr : &*it;
}

const case_desc *type_desc::find_case(std::uint32_t no) const noexcept {
  const auto it = std::ranges::find(cases, no, &case_desc::no);
  return it == cases.end() ? nullptr : &*it;
}

type_id type_id_of(std::string_view qualified_name) noexcept {
  static const record_id root = well_known(id_class::namespace_, 1);
  const record_id id = derive(id_class::type, root, qualified_name);
  // A derived id of class type always converts.
  return *type_id::from(id);
}

std::span<const type_desc> builtin_types() {
  static const std::vector<type_desc> builtins = make_builtins();
  return builtins;
}

bool is_builtin_type(const type_id &id) noexcept {
  return std::ranges::any_of(builtin_types(), [&](const type_desc &t) { return t.id == id; });
}

result<void> check_well_formed(const type_desc &t) {
  // R-TY-NAME
  if (!is_qualified_name(t.name)) {
    return invalid(t, "R-TY-NAME", "the name is not dot-separated identifiers");
  }
  for (const field_desc &f : t.fields) {
    if (!is_identifier(f.name)) {
      return invalid(t, "R-TY-NAME", std::format("field {} name '{}'", f.no, f.name));
    }
  }
  for (const enumerator_desc &e : t.enumerators) {
    if (!is_identifier(e.name)) {
      return invalid(t, "R-TY-NAME", std::format("enumerator {} name '{}'", e.no, e.name));
    }
  }
  for (const case_desc &c : t.cases) {
    if (!is_identifier(c.name)) {
      return invalid(t, "R-TY-NAME", std::format("case {} name '{}'", c.no, c.name));
    }
  }
  // R-TY-ID
  if (t.id != type_id_of(t.name)) {
    return invalid(t, "R-TY-ID", "the id is not type_id_of(name)");
  }
  // R-TY-VERSION
  if (t.version.major < 1) {
    return invalid(t, "R-TY-VERSION", "major version 0");
  }
  // R-TY-SHAPE
  const bool structish = t.kind == type_kind::struct_ || t.kind == type_kind::external_ref;
  const bool has_element = t.kind == type_kind::list || t.kind == type_kind::oset ||
                           t.kind == type_kind::map || t.kind == type_kind::tagged;
  if (!type_kind_from_string(to_string(t.kind))) {
    return invalid(t, "R-TY-SHAPE", "unknown kind");
  }
  if (is_builtin_kind(t.kind) && !is_builtin_type(t.id)) {
    return invalid(
        t, "R-TY-SHAPE",
        std::format("kind {} is built in; declare a tagged type instead", to_string(t.kind)));
  }
  if (!structish && (!t.fields.empty() || !t.reserved.empty())) {
    return invalid(t, "R-TY-SHAPE", "fields or reserved numbers on a non-struct");
  }
  if (t.kind != type_kind::enum_ && !t.enumerators.empty()) {
    return invalid(t, "R-TY-SHAPE", "enumerators on a non-enum");
  }
  if (t.kind != type_kind::variant && !t.cases.empty()) {
    return invalid(t, "R-TY-SHAPE", "cases on a non-variant");
  }
  if (has_element != t.element.has_value() || (t.element && t.element->is_null())) {
    return invalid(t, "R-TY-SHAPE",
                   has_element ? "the element type is missing" : "an element type on this kind");
  }
  if ((t.kind == type_kind::map) != t.key.has_value() || (t.key && t.key->is_null())) {
    return invalid(t, "R-TY-SHAPE",
                   t.kind == type_kind::map ? "the key type is missing"
                                            : "a key type on a non-map");
  }
  // R-TY-FIELD
  for (const field_desc &f : t.fields) {
    if (f.no == 0) {
      return invalid(t, "R-TY-FIELD", std::format("field '{}' has number 0", f.name));
    }
    if (f.type.is_null()) {
      return invalid(t, "R-TY-FIELD", std::format("field {} has no type", f.no));
    }
  }
  if (has_duplicates(t.fields, &field_desc::no)) {
    return invalid(t, "R-TY-FIELD", "a field number repeats");
  }
  if (has_duplicates(t.fields, &field_desc::name)) {
    return invalid(t, "R-TY-FIELD", "a field name repeats");
  }
  // R-TY-RESERVED
  if (has_duplicates(t.reserved, std::identity{})) {
    return invalid(t, "R-TY-RESERVED", "a reserved number repeats");
  }
  for (const std::uint32_t r : t.reserved) {
    if (t.find_field(r) != nullptr) {
      return invalid(t, "R-TY-RESERVED", std::format("reserved number {} is a field", r));
    }
  }
  for (const field_desc &f : t.fields) {
    // R-TY-DEFAULT
    if (f.default_value && f.presence != presence::optional) {
      return invalid(t, "R-TY-DEFAULT", std::format("required field {} has a default", f.no));
    }
    // R-TY-SINCE
    if (f.since_minor > t.version.minor) {
      return invalid(
          t, "R-TY-SINCE",
          std::format("field {} since minor {} is after {}", f.no, f.since_minor, t.version.minor));
    }
    if (f.deprecated_in &&
        (*f.deprecated_in < f.since_minor || *f.deprecated_in > t.version.minor)) {
      return invalid(t, "R-TY-SINCE",
                     std::format("field {} deprecated in minor {}", f.no, *f.deprecated_in));
    }
    // R-TY-REF
    if (f.ref) {
      if (f.ref->targets.empty()) {
        return invalid(t, "R-TY-REF", std::format("field {} ref has no target", f.no));
      }
      if (has_duplicates(f.ref->targets, std::identity{})) {
        return invalid(t, "R-TY-REF", std::format("field {} ref names a target twice", f.no));
      }
      const auto s = static_cast<std::uint8_t>(f.ref->strength);
      if (s < 1 || s > 3) {
        return invalid(t, "R-TY-REF", std::format("field {} ref strength {}", f.no, s));
      }
    }
  }
  // R-TY-ENUM
  if (t.kind == type_kind::enum_) {
    if (t.enumerators.empty()) {
      return invalid(t, "R-TY-ENUM", "no enumerators");
    }
    if (has_duplicates(t.enumerators, &enumerator_desc::no) ||
        has_duplicates(t.enumerators, &enumerator_desc::name)) {
      return invalid(t, "R-TY-ENUM", "an enumerator number or name repeats");
    }
  }
  // R-TY-CASE
  if (t.kind == type_kind::variant) {
    if (t.cases.empty()) {
      return invalid(t, "R-TY-CASE", "no cases");
    }
    if (has_duplicates(t.cases, &case_desc::no) || has_duplicates(t.cases, &case_desc::name)) {
      return invalid(t, "R-TY-CASE", "a case number or name repeats");
    }
    for (const case_desc &c : t.cases) {
      if (c.type.is_null()) {
        return invalid(t, "R-TY-CASE", std::format("case {} has no type", c.no));
      }
    }
  }
  return {};
}

bool is_valid_order_key(byte_span key) noexcept {
  return !key.empty() && key.size() <= max_byte_length && key.back() != 0;
}

result<std::vector<std::uint8_t>> order_key_between(byte_span lo, byte_span hi) {
  if ((!lo.empty() && !is_valid_order_key(lo)) || (!hi.empty() && !is_valid_order_key(hi))) {
    return make_error(errc::invalid_argument, "order_key_between: a bound is not a valid key");
  }
  if (!lo.empty() && !hi.empty() && !std::ranges::lexicographical_compare(lo, hi)) {
    return make_error(errc::invalid_argument, "order_key_between: lo is not below hi");
  }
  // Keys are base-256 fractions 0.k1k2... (no trailing zero, so each fraction has one key):
  // copy the common digits, then take a digit strictly between the bounds, or copy lo's digit
  // and continue with the upper bound open.
  std::vector<std::uint8_t> out;
  bool hi_open = hi.empty();
  for (std::size_t i = 0;; ++i) {
    const unsigned a = i < lo.size() ? lo[i] : 0U;
    unsigned b = 256U;
    if (!hi_open) {
      b = i < hi.size() ? hi[i] : 0U;
    }
    if (a == b) {
      out.push_back(static_cast<std::uint8_t>(a));
      continue;
    }
    if (b - a > 1) {
      out.push_back(static_cast<std::uint8_t>((a + b) / 2));
      return out;
    }
    out.push_back(static_cast<std::uint8_t>(a));
    hi_open = true;
  }
}

std::optional<value> read_field(const value &s, const field_desc &f) {
  if (const value *v = s.find_field(f.no)) {
    return *v;
  }
  return f.default_value;
}

} // namespace sl::fabric
