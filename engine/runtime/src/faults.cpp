// The failpoint registry (WO-076, SL-ARCH-003 ARCH3-R008/R020 and `failpoints`, ADR-0022). The
// table is constexpr data; the registry, one slot per failpoint, is the only mutable
// namespace-scope state of the runtime (allow-listed in tools/runtime/lint_allow.json, symbol
// fault_registry). Every access is a lock-free atomic. Compiled out (no registry, visit() a
// constexpr no-op in faults.hpp) when SL_RUNTIME_FAULTS=0.

#include "sl/runtime/faults.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace sl::runtime {

namespace {

constexpr auto failpoints = std::to_array<failpoint_info>({
    {.id = "RFP0",
     .name = "alloc.fail",
     .site = "mem::resource::try_allocate (Nth call or per tag)"},
    {.id = "RFP1", .name = "thread.spawn", .site = "platform thread_factory::spawn"},
    {.id = "RFP2", .name = "job.error", .site = "jobs::job_system before running a job body"},
    {.id = "RFP3", .name = "job.fatal", .site = "jobs::job_system before running a job body"},
    {.id = "RFP4",
     .name = "worker.stall",
     .site = "jobs worker loop at before_pop, after_steal, before_complete and in_wait"},
    {.id = "RFP5", .name = "system.fail", .site = "ecs::system_scheduler before a system body"},
    {.id = "RFP6", .name = "clock.jump", .site = "time::frame_clock::sample"},
    {.id = "RFP7", .name = "event.overflow", .site = "event_channel<E>::write"},
    {.id = "RFP8",
     .name = "snapshot.io",
     .site = "snapshot::save through file_access::atomic_write, after each storage primitive"},
    {.id = "RFP9",
     .name = "suspend.request",
     .site = "runtime at each boundary slot and mid-phase"},
    {.id = "RFP10", .name = "entity.exhaust", .site = "ecs::entity_allocator::create"},
});
static_assert(failpoints.size() == failpoint_count);

#if SL_RUNTIME_FAULTS

// One failpoint. `target` is the visit number (1-based, counted since process start) at which the
// point fires, 0 when disarmed; a sticky point also fires at every later visit.
struct alignas(64) fault_slot {
  std::atomic<std::uint64_t> visits{0};
  std::atomic<std::uint64_t> target{0};
  std::atomic<std::uint32_t> param{0};
  std::atomic<bool> sticky{false};
};

// The failpoint registry: the one allow-listed mutable state of the runtime (ARCH3-R008). A
// function-local constinit object: constant-initialized (no guard, no order problem) and
// reachable only through this function.
std::array<fault_slot, failpoint_count> &registry() noexcept {
  static constinit std::array<fault_slot, failpoint_count> fault_registry{};
  return fault_registry;
}

// Null for a value outside rfp0..rfp10.
fault_slot *slot_of(failpoint point) noexcept {
  const auto i = static_cast<std::size_t>(point);
  if (i >= failpoint_count) {
    return nullptr;
  }
  const std::span<fault_slot, failpoint_count> slots(registry());
  return &slots[i];
}

#endif

} // namespace

std::span<const failpoint_info> failpoint_table() noexcept { return failpoints; }

#if SL_RUNTIME_FAULTS

fault_action visit(failpoint point) noexcept {
  fault_slot *s = slot_of(point);
  if (s == nullptr) {
    return {};
  }
  // pairs: visits(); joins and arm's load order the count, the counter itself needs no fence
  const std::uint64_t n = s->visits.fetch_add(1, std::memory_order_relaxed) + 1;
  // pairs: fault_registry_hooks::arm (release store of target after param and sticky)
  const std::uint64_t target = s->target.load(std::memory_order_acquire);
  if (target == 0 || n < target) {
    return {};
  }
  // pairs: fault_registry_hooks::arm (ordered by the acquire load of target above)
  if (n == target || s->sticky.load(std::memory_order_relaxed)) {
    // pairs: fault_registry_hooks::arm (ordered by the acquire load of target above)
    return {.fire = true, .param = s->param.load(std::memory_order_relaxed)};
  }
  return {};
}

std::uint64_t visits(failpoint point) noexcept {
  const fault_slot *s = slot_of(point);
  // pairs: visit (relaxed fetch_add; callers read the count after joining the visiting threads)
  return s == nullptr ? 0 : s->visits.load(std::memory_order_relaxed);
}

void detail::fault_registry_hooks::arm(failpoint point, std::uint64_t at_visit, bool sticky,
                                       std::uint32_t param) noexcept {
  fault_slot *s = slot_of(point);
  if (s == nullptr || at_visit == 0) {
    return;
  }
  // pairs: visit (acquire load of target): disarm first so no visit sees new options, old target
  s->target.store(0, std::memory_order_release);
  // pairs: visit (published by the release store of target below)
  s->param.store(param, std::memory_order_relaxed);
  // pairs: visit (published by the release store of target below)
  s->sticky.store(sticky, std::memory_order_relaxed);
  // pairs: visit (relaxed fetch_add of visits; arming happens before the workload starts)
  const std::uint64_t base = s->visits.load(std::memory_order_relaxed);
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t target = at_visit > max - base ? max : base + at_visit;
  // pairs: visit (acquire load of target)
  s->target.store(target, std::memory_order_release);
}

void detail::fault_registry_hooks::disarm(failpoint point) noexcept {
  fault_slot *s = slot_of(point);
  if (s == nullptr) {
    return;
  }
  // pairs: visit (acquire load of target)
  s->target.store(0, std::memory_order_release);
  // pairs: visit (only read after a non-zero target)
  s->sticky.store(false, std::memory_order_relaxed);
  // pairs: visit (only read after a non-zero target)
  s->param.store(0, std::memory_order_relaxed);
}

#else

// Compiled out: nothing to arm (testing::arm returns unsupported before reaching these).
void detail::fault_registry_hooks::arm(failpoint /*point*/, std::uint64_t /*at_visit*/,
                                       bool /*sticky*/, std::uint32_t /*param*/) noexcept {}

void detail::fault_registry_hooks::disarm(failpoint /*point*/) noexcept {}

#endif

} // namespace sl::runtime
