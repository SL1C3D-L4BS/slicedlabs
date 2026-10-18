#pragma once

// An in-memory record_enumerator for the dependency tests (WO-044): the records of a project as the
// graph reads them, and seeded random graphs (edges over ids of mixed classes) for the oracle
// tests.

#include <cstddef>
#include <map>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "value_gen.hpp"

namespace sl::fabric::testing {

class model_store final : public record_enumerator {
public:
  /// Adds or replaces a record. `invalid_argument` for a null or invalid id.
  [[nodiscard]] result<void> put(const record_id &id, type_id type, value body,
                                 std::vector<record_id> provenance_inputs = {});
  void erase(const record_id &id);
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

  [[nodiscard]] result<void> for_each(const visitor &fn) const override;

private:
  struct entry {
    type_id type;
    value body;
    std::vector<record_id> inputs;
  };
  std::map<record_id, entry> records_;
};

/// `count` distinct ids, classes cycling through asset, entity, instance, derived: stable names.
[[nodiscard]] std::vector<record_id> graph_ids(std::size_t count);

/// Up to `count` distinct random edges between `ids` (self loops included), each of a random
/// kind drawn from `kinds`; strength follows the kind.
[[nodiscard]] std::vector<edge> random_edges(rng &r, const std::vector<record_id> &ids,
                                             std::size_t count, edge_kind_mask kinds);

/// A graph in which record ids[i] owns the edges that start at it.
[[nodiscard]] result<dependency_graph> graph_of(const std::vector<record_id> &ids,
                                                const std::vector<edge> &edges);

} // namespace sl::fabric::testing
