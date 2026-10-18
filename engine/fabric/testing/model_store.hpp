#pragma once

// A trivially correct record store for the fabric tests (WO-043; used by WO-044, WO-046 and
// later as the oracle).
//
// The class is oracle_store, not model_store: WO-044's testing/deps_model_store.hpp declares a
// testing::model_store (a record_enumerator) in the same library, and two classes of one name
// would break the one-definition rule.
//
// oracle_store is a std::map from record id to {type, body} with its own plain implementation
// of the patch ops: it shares the value primitives (with, without, insert_element,
// move_element) with the engine but none of the patch code, so the engine's staged_state,
// normalize and algebra are checked against it.

#include <cstddef>
#include <map>
#include <optional>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

class oracle_store final : public state_reader {
public:
  [[nodiscard]] result<std::optional<record_state>> read(const record_id &id) const override;

  /// Puts a record directly (fixtures); replaces one with the same id.
  void put(const record_id &id, record_state r);
  void erase(const record_id &id);
  /// Applies every op in order with the model's own semantics, checking filled expectations;
  /// all or nothing.
  [[nodiscard]] result<void> apply(const patch &p);
  [[nodiscard]] result<void> apply(const patch_op &op);

  [[nodiscard]] const std::map<record_id, record_state> &records() const noexcept {
    return records_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

  /// SHA-256 over a canonical walk of every record in id order (id, type, then the body: kind,
  /// sizes and contents in canonical container order). A test fingerprint, not the SLB/1 root
  /// of WO-046: equal stores have equal digests.
  [[nodiscard]] digest root_digest() const;

  friend bool operator==(const oracle_store &a, const oracle_store &b) noexcept {
    return a.records_ == b.records_;
  }

private:
  std::map<record_id, record_state> records_;
};

/// The oracle_store holding `base` with every change of `staged` applied over it.
[[nodiscard]] oracle_store materialize(const oracle_store &base, const staged_state &staged);

} // namespace sl::fabric::testing
