// Runtime observations as proposed patches (WO-061, SL-ARCH-001 ARCH1-R60, FAB-INV-BND-01).

#include <span>
#include <string>
#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/transaction.hpp"

namespace sl::fabric {

result<patch> propose_from_observation(const document_projection &projection,
                                       std::span<const observed_change> changes,
                                       const tx_origin &origin) {
  if (origin.epistemic != epistemic_class::observed) {
    return make_error(errc::invalid_argument,
                      "an observation is proposed with epistemic class observed, not " +
                          std::string(to_string(origin.epistemic)));
  }
  if (changes.empty()) {
    return make_error(errc::invalid_argument, "an observation proposes at least one change");
  }
  patch p;
  p.base = projection.revision_digest();
  p.meta.label = std::string(observation_label);
  // Scope first: a record outside the projection never gets as far as the patch layer.
  for (const observed_change &c : changes) {
    if (c.record.is_null()) {
      return make_error(errc::invalid_argument, "an observed change names the null record id");
    }
    if (auto in_scope = projection.check_scope(c.record); !in_scope) {
      return std::unexpected(std::move(in_scope.error()));
    }
    p.ops.push_back(op_set(c.record, c.path, c.value));
  }
  return normalize(p, projection);
}

} // namespace sl::fabric
