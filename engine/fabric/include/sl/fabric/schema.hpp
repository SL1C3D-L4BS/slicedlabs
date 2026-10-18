#pragma once

// The schema registry (WO-039, SL-ARCH-001 ARCH1-R12 to ARCH1-R18, ADR-0013).
//
// A registry starts with the built-in types (type.hpp builtin_types), takes types with add(),
// and is frozen once (ARCH1-R38: before the first workspace opens). add() checks one type on its
// own (type.hpp check_well_formed) and refuses a repeated id; after freeze() it is
// `schema_frozen`. freeze() checks the whole set and refuses with `schema_invalid`, listing every
// violation, sorted by type name then rule, and stays unfrozen:
//   R-TY-RECORD      a record type without an assigned record class
//   R-TY-DOMAIN      a record type without a domain (FAB-INV-TY-05)
//   R-TY-STATE       a record type whose state class is not document, derived or observation
//   R-TY-UNRESOLVED  a field, element, key, case or ref target type that is not registered
//   R-TY-KEY         a map key type that is not a string, integer or id type (through tagged)
//   R-TY-REF         a ref target that is not a record type; a field whose type reaches a ref
//                    (through tagged, list, map value and oset element) without a ref spec, or a
//                    ref spec on a field that reaches no ref; a variant case that reaches a ref
//   R-TY-RECURSION   a cycle through required fields, variant cases and tagged bases only
//                    (recursion must pass an optional field, list, map or oset: FAB-INV-TY-01)
//   R-TY-LIST-ID     a list whose element is an identity-bearing struct: a record type or a struct
//                    with a field of kind id; identity-bearing collections are osets (ARCH1-R15,
//                    FAB-INV-TY-06)
//   R-TY-DEFAULT     a default value that does not conform to its field's type
//
// check() validates a value against a type of a frozen registry and appends one diagnostic per
// violation, sorted by (path, rule, message) (FAB-INV-TY-02). Rules:
//   R-TY-KIND        the value's kind does not match the type's kind
//   R-TY-RANGE       an integer outside the width of i8..u64
//   R-TY-REQUIRED    a required field is absent
//   R-TY-FIELD       a struct holds a field number its type does not declare
//   R-TY-RESERVED    a struct holds a reserved field number
//   R-TY-ENUM        a uint that is no enumerator of the enum
//   R-TY-CASE        a variant case the type does not declare
//   R-TY-ID          an id or ref that is null or has an unassigned class or scheme
//   R-TY-REF-TARGET  a ref whose id class is the record class of none of its field's targets
//   R-TY-ELEMENT     an oset element id that is not a valid id of class element
//   R-TY-ORDER-KEY   an oset order key that is not valid (type.hpp is_valid_order_key)
// A diagnostic inside a list carries the path of the list (lists are atomic) and the index in its
// message.

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// One conformance violation: where, which rule, and a human message.
struct diagnostic_lite {
  fabric::path path;
  std::string rule;
  std::string message;

  friend bool operator==(const diagnostic_lite &, const diagnostic_lite &) = default;
  friend auto operator<=>(const diagnostic_lite &a, const diagnostic_lite &b) {
    if (const auto c = a.path <=> b.path; c != 0) {
      return c;
    }
    if (const auto c = a.rule <=> b.rule; c != 0) {
      return c;
    }
    return a.message <=> b.message;
  }
};

class schema_registry {
public:
  /// A registry holding the built-in types.
  schema_registry();

  /// `schema_frozen` after freeze; `schema_invalid` for a type that breaks check_well_formed or
  /// whose id is already registered (R-TY-DUP).
  [[nodiscard]] result<void> add(type_desc t);
  /// Checks the whole set (rules above) and freezes it; `schema_invalid` listing every
  /// violation otherwise. Freezing a frozen registry succeeds.
  [[nodiscard]] result<void> freeze();
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  [[nodiscard]] const type_desc *find(type_id id) const noexcept;
  [[nodiscard]] const type_desc *find(std::string_view qualified_name) const noexcept;
  /// Every type (built-ins included), in type id order.
  [[nodiscard]] const std::map<type_id, type_desc> &types() const noexcept { return types_; }
  [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }

  /// Validates `v` against `type`, appending the diagnostics, sorted, to `out`. `schema_invalid`
  /// before freeze, `schema_unknown` for an unregistered type, `type_mismatch` when any
  /// diagnostic was produced; success otherwise.
  [[nodiscard]] result<void> check(const value &v, type_id type,
                                   std::vector<diagnostic_lite> &out) const;

private:
  std::map<type_id, type_desc> types_;
  bool frozen_ = false;
};

} // namespace sl::fabric
