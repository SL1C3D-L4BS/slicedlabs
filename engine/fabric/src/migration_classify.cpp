// The compatibility classifier (WO-057, SL-ARCH-001 ARCH1-R21): the C++ twin of
// tools/fabric/schema_compat.py `changes`/`classify`. Each Python change code maps to its class;
// classify keeps the worst one.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

namespace {

struct int_range {
  std::string_view name;
  std::int64_t lo;
  std::uint64_t hi;
};

// schemalib.INT_RANGES.
constexpr std::array<int_range, 8> int_ranges{{
    {.name = "i8",
     .lo = std::numeric_limits<std::int8_t>::min(),
     .hi = std::numeric_limits<std::int8_t>::max()},
    {.name = "i16",
     .lo = std::numeric_limits<std::int16_t>::min(),
     .hi = std::numeric_limits<std::int16_t>::max()},
    {.name = "i32",
     .lo = std::numeric_limits<std::int32_t>::min(),
     .hi = std::numeric_limits<std::int32_t>::max()},
    {.name = "i64",
     .lo = std::numeric_limits<std::int64_t>::min(),
     .hi = std::numeric_limits<std::int64_t>::max()},
    {.name = "u8", .lo = 0, .hi = std::numeric_limits<std::uint8_t>::max()},
    {.name = "u16", .lo = 0, .hi = std::numeric_limits<std::uint16_t>::max()},
    {.name = "u32", .lo = 0, .hi = std::numeric_limits<std::uint32_t>::max()},
    {.name = "u64", .lo = 0, .hi = std::numeric_limits<std::uint64_t>::max()},
}};

const int_range *int_range_of(const std::optional<type_id> &t) noexcept {
  if (!t) {
    return nullptr;
  }
  for (const int_range &r : int_ranges) {
    if (type_id_of(r.name) == *t) {
      return &r;
    }
  }
  return nullptr;
}

// schema_compat._int_change: widen_int (backward_only), narrow_int or change_field_type (breaking).
compat int_change(const std::optional<type_id> &from, const std::optional<type_id> &to) noexcept {
  const int_range *a = int_range_of(from);
  const int_range *b = int_range_of(to);
  if (a != nullptr && b != nullptr && b->lo <= a->lo && b->hi >= a->hi) {
    return compat::backward_only;
  }
  return compat::breaking;
}

class worst {
public:
  void note(compat c) noexcept { cls_ = std::max(cls_, c); }
  [[nodiscard]] compat get() const noexcept { return cls_; }

private:
  compat cls_ = compat::identical;
};

bool contains(const std::vector<std::uint32_t> &v, std::uint32_t no) noexcept {
  return std::ranges::find(v, no) != v.end();
}

// schema_compat._ref_changes.
void ref_changes(const std::optional<ref_spec> &from, const std::optional<ref_spec> &to,
                 worst &w) noexcept {
  if (!from || !to) {
    w.note(compat::breaking); // a ref declaration appeared or vanished: change_field_type
    return;
  }
  if (from->strength != to->strength) {
    w.note(from->strength == ref_strength::strong && to->strength == ref_strength::weak
               ? compat::additive
               : compat::breaking);
  }
  for (const type_id &t : to->targets) {
    if (std::ranges::find(from->targets, t) == from->targets.end()) {
      w.note(compat::additive); // add_ref_target
    }
  }
  for (const type_id &t : from->targets) {
    if (std::ranges::find(to->targets, t) == to->targets.end()) {
      w.note(compat::breaking); // remove_ref_target
    }
  }
}

// schema_compat._field_changes (label and deprecate codes are identical and leave w unchanged).
void field_changes(const field_desc &from, const field_desc &to, worst &w) noexcept {
  if (from.type != to.type) {
    w.note(int_change(from.type, to.type));
  } else if (from.type == type_id_of("ref")) {
    ref_changes(from.ref, to.ref, w);
  }
  if (from.presence != to.presence) {
    w.note(from.presence == presence::required ? compat::backward_only : compat::breaking);
  }
  if (from.default_value != to.default_value) {
    w.note(compat::breaking); // change_default
  }
}

// schema_compat._struct_changes.
void struct_changes(const type_desc &from, const type_desc &to, worst &w) noexcept {
  for (const field_desc &f : to.fields) {
    if (from.find_field(f.no) != nullptr) {
      continue;
    }
    if (contains(from.reserved, f.no) || contains(to.reserved, f.no)) {
      w.note(compat::forbidden); // reuse_reserved
    }
    w.note(f.presence == presence::optional ? compat::additive : compat::breaking);
  }
  for (const field_desc &f : from.fields) {
    const field_desc *now = to.find_field(f.no);
    if (now == nullptr) {
      if (!contains(to.reserved, f.no)) {
        w.note(compat::forbidden); // remove_unreserved
      }
      w.note(f.presence == presence::optional ? compat::backward_only : compat::breaking);
      continue;
    }
    field_changes(f, *now, w);
  }
  for (const std::uint32_t no : from.reserved) {
    if (!contains(to.reserved, no)) {
      w.note(compat::forbidden); // a reserved number dropped from the list
    }
  }
  for (const std::uint32_t no : to.reserved) {
    if (to.find_field(no) != nullptr && from.find_field(no) != nullptr) {
      w.note(compat::forbidden); // a field number both a field and reserved
    }
  }
}

// schema_compat._members over enumerators: add additive, remove breaking, the rest labels.
void enumerator_changes(const type_desc &from, const type_desc &to, worst &w) noexcept {
  for (const enumerator_desc &e : to.enumerators) {
    if (from.find_enumerator(e.no) == nullptr) {
      w.note(compat::additive);
    }
  }
  for (const enumerator_desc &e : from.enumerators) {
    if (to.find_enumerator(e.no) == nullptr) {
      w.note(compat::breaking);
    }
  }
}

// schema_compat._members over cases: as enumerators, and a case type change is an int change.
void case_changes(const type_desc &from, const type_desc &to, worst &w) noexcept {
  for (const case_desc &c : to.cases) {
    if (from.find_case(c.no) == nullptr) {
      w.note(compat::additive);
    }
  }
  for (const case_desc &c : from.cases) {
    const case_desc *now = to.find_case(c.no);
    if (now == nullptr) {
      w.note(compat::breaking);
    } else if (now->type != c.type) {
      w.note(int_change(c.type, now->type));
    }
  }
}

} // namespace

std::string_view to_string(compat c) noexcept {
  switch (c) {
  case compat::identical:
    return "identical";
  case compat::additive:
    return "additive";
  case compat::backward_only:
    return "backward_only";
  case compat::breaking:
    return "breaking";
  case compat::forbidden:
    return "forbidden";
  }
  return "unknown";
}

compat classify(const type_desc &from, const type_desc &to) noexcept {
  if (from.name != to.name) {
    return compat::forbidden;
  }
  worst w;
  if (from.domain != to.domain || from.state_class != to.state_class ||
      from.record_class != to.record_class) {
    w.note(compat::forbidden);
  }
  if (from.document_root != to.document_root) {
    w.note(compat::breaking);
  }
  if (from.kind != to.kind) {
    w.note(compat::breaking); // list_oset_swap or change_field_type: both breaking
    return w.get();
  }
  switch (from.kind) {
  case type_kind::struct_:
    struct_changes(from, to, w);
    break;
  case type_kind::enum_:
    enumerator_changes(from, to, w);
    break;
  case type_kind::variant:
    case_changes(from, to, w);
    break;
  default:
    break;
  }
  if (from.element != to.element) {
    w.note(int_change(from.element, to.element));
  }
  if (from.key != to.key) {
    w.note(int_change(from.key, to.key));
  }
  return w.get();
}

} // namespace sl::fabric
