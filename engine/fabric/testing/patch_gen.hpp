#pragma once

// Seeded random record stores and patches over every op kind (WO-043; used by WO-046, WO-050,
// WO-052 and later).
//
// Bodies are structs holding scalars, nested structs, string-keyed maps, osets (element ids of
// class element, valid order keys, equal keys on purpose so ties are exercised), variants and
// atomic lists. A generated patch applies to the store it was generated for: each op is valid
// where it stands. Expectations are left for normalize() except for a share the "author" fills
// correctly. Every draw comes from the value_gen splitmix64 stream, so a seed reproduces.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "model_store.hpp"
#include "value_gen.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

struct patch_gen_options {
  std::size_t max_ops = 4;         // ops per patch (at least 1)
  std::size_t max_path_depth = 4;  // path segments an op may address
  unsigned prefill_percent = 25;   // ops whose expectations the author fills
  unsigned tie_percent = 25;       // inserts and moves reusing a sibling's order key
  unsigned meta_percent = 50;      // patches with a base and non-empty meta
  unsigned whole_body_percent = 3; // sets of the root path
  std::size_t type_pool = 4;       // record types "gen.rec0".."gen.rec<n-1>"
};

/// One addressable node of a body: its path, value and the kind of its parent.
struct body_node {
  fabric::path path;
  value v;
  value_kind parent = value_kind::struct_; // meaningless for the root
};

class patch_gen {
public:
  explicit patch_gen(std::uint64_t seed, patch_gen_options options = {});

  /// A store of `records` random records.
  [[nodiscard]] result<oracle_store> state(std::size_t records);
  /// A random patch that applies to `state`; op kinds are drawn uniformly among the possible.
  [[nodiscard]] result<patch> next(const oracle_store &state);
  /// One random op of `kind` that applies to `state`; nullopt when none can (no record, no
  /// oset, ...).
  [[nodiscard]] result<std::optional<patch_op>> op(const oracle_store &state, op_kind kind);

  /// A random record body (a struct).
  [[nodiscard]] value body();
  /// A random value of at most `levels` nested containers.
  [[nodiscard]] value node(std::size_t levels);
  /// A random record type id from the pool.
  [[nodiscard]] type_id type();
  /// A random patch meta, sometimes empty.
  [[nodiscard]] patch_meta meta();
  /// Every node of `body` up to max_path_depth segments, in walk order (lists are not entered).
  [[nodiscard]] std::vector<body_node> nodes(const value &body) const;

  [[nodiscard]] value_gen &values() noexcept { return values_; }
  [[nodiscard]] rng &random() noexcept { return values_.random(); }
  [[nodiscard]] const patch_gen_options &options() const noexcept { return options_; }

private:
  value make(value_kind kind, std::size_t levels);
  std::vector<std::uint8_t> key_near(const value &oset);
  bool prefill() noexcept;

  value_gen values_; // max_depth 0: scalars only
  patch_gen_options options_;
};

} // namespace sl::fabric::testing
