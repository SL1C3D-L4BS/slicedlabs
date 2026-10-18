// testing::backdoor (WO-042, ADR-0016): the test-only maker of a mutation_capability. It is
// declared in sl/fabric/store.hpp and defined only here, in sl_fabric_testing, so production
// code (which links sl_fabric alone) cannot reach a commit without transaction_manager.

#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric::testing {

result<commit_ack> backdoor::commit(store_backend &store, commit_batch batch) noexcept {
  const mutation_capability cap{};
  return store.commit(cap, std::move(batch));
}

} // namespace sl::fabric::testing
