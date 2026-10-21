// The selection step of merge_ordered (WO-084, SL-ARCH-003 ARCH3-R086, `determinism`
// merge_points): the smallest head key wins; keys compare lexicographically, never by buffer,
// pointer, thread or completion order, so the merged sequence depends on the keys alone. A key
// that appears at the head of two buffers is a Debug assertion (keys are unique per tick).

#include "sl/core/assert.hpp"
#include "sl/runtime/replay.hpp"

#include <cstddef>
#include <span>

namespace sl::runtime::det::detail {

std::size_t min_head(std::span<const ordered_key *const> heads) noexcept {
  std::size_t best = heads.size();
  for (std::size_t i = 0; i < heads.size(); ++i) {
    const ordered_key *k = heads[i];
    if (k == nullptr) {
      continue;
    }
    if (best == heads.size() || *k < *heads[best]) {
      best = i;
    } else {
      SL_DEBUG_ASSERT(*k != *heads[best], "merge_ordered: a key repeats across buffers");
    }
  }
  return best;
}

} // namespace sl::runtime::det::detail
