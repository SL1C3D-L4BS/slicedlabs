// The observation_flush slot of the link (WO-097, SL-ARCH-003 ARCH3-R155, R028; ADR-0027).

#include "sl/runtime/bridge.hpp"

#include "sl/fabric/boundary.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/runtime/ecs/system.hpp"

#include <cstdint>
#include <vector>

namespace sl::runtime::link::detail {

namespace fab = sl::fabric;

void flush_observations(link_state &s) noexcept {
  check_alive(s);
  // A faulted frame flushes nothing (ARCH3-R028): a system failed or a playback failed since the
  // previous flush.
  const std::uint64_t failures = s.systems->failures();
  const bool faulted = failures != s.failures_seen || !s.systems->last_flush().has_value();
  s.failures_seen = failures;
  if (faulted) {
    s.out.clear();
    return;
  }
  if (s.out.pending() == 0) {
    return;
  }
  const std::vector<fab::observed_change> changes = s.out.drain();
  if (!s.current) {
    ++s.stats.rejected;
    return;
  }
  fab::tx_origin origin;
  origin.actor = s.config.actor;
  origin.subsystem = s.config.subsystem;
  origin.epistemic = fab::epistemic_class::observed;
  auto proposal = fab::propose_from_observation(*s.current, changes, origin);
  if (!proposal) {
    ++s.stats.rejected; // link_observation_rejected: counted, never a frame error
    return;
  }
  ++s.stats.proposed_patches;
  s.stats.proposed_changes += changes.size();
  if (auto taken = s.sink->submit(*proposal, origin); !taken) {
    ++s.stats.rejected;
  }
}

} // namespace sl::runtime::link::detail
