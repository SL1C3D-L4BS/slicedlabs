#pragma once

// Seeded random types and values for the fabric tests (WO-039; used by WO-041 and later).
//
// Every draw comes from splitmix64 and bounded integer arithmetic, never from <random>
// distributions, so a seed gives the same types and values on every compiler and platform.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

/// splitmix64.
class rng {
public:
  explicit rng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept;
  /// Uniform in [0, n); 0 when n is 0.
  [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept;
  /// True with probability percent / 100.
  [[nodiscard]] bool chance(unsigned percent) noexcept { return below(100) < percent; }

private:
  std::uint64_t state_;
};

struct value_gen_options {
  std::size_t max_depth = 4;  // nested containers (values) and nested composite types
  std::size_t max_items = 4;  // items per container, fields per struct
  std::size_t max_chars = 12; // characters of a string, bytes of a bytes value
};

class value_gen {
public:
  explicit value_gen(std::uint64_t seed, value_gen_options options = {}) noexcept
      : rng_(seed), options_(options) {}

  /// Any SLB/1 value: every kind, boundary integers, multi-byte UTF-8, raw (possibly invalid)
  /// ids, any order keys and field numbers up to 2^32 - 1.
  [[nodiscard]] value any();

  /// Adds a random well-formed type, and the types it is built from, to the unfrozen `reg`;
  /// names are "gen.t<n>". Generated types hold no refs and are not record types.
  [[nodiscard]] result<type_id> add_type(schema_registry &reg);

  /// A random value that conforms to `type` (every type it reaches must be registered).
  [[nodiscard]] result<value> conforming(const schema_registry &reg, type_id type);

  /// A random valid id of class `c`.
  [[nodiscard]] record_id random_id(id_class c);
  /// A random valid order key.
  [[nodiscard]] std::vector<std::uint8_t> random_order_key();

  [[nodiscard]] rng &random() noexcept { return rng_; }

private:
  result<type_id> gen_type(schema_registry &reg, std::size_t depth, bool in_struct);
  result<value> gen_value(const schema_registry &reg, const type_desc *t, const ref_spec *ref,
                          std::size_t depth);
  value any_scalar();
  value any_at(std::size_t depth);

  rng rng_;
  value_gen_options options_;
  std::uint64_t next_name_ = 0;
};

} // namespace sl::fabric::testing
