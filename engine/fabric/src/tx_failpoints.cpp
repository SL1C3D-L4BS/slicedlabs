// Commit failpoints FP0-FP6 (WO-050, SL-ARCH-002 ARCH2-R19 and `failpoints`, ADR-0015).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/failpoints.hpp"

namespace sl::fabric {

namespace {

constexpr std::array<failpoint_info, failpoint_count> table{{
    {.fp = failpoint::fp0,
     .name = "FP0",
     .at = "before validate",
     .outcome = "not committed",
     .committed = false,
     .manager_visits = true},
    {.fp = failpoint::fp1,
     .name = "FP1",
     .at = "after validate",
     .outcome = "not committed",
     .committed = false,
     .manager_visits = true},
    {.fp = failpoint::fp2,
     .name = "FP2",
     .at = "after materialise",
     .outcome = "not committed; objects in memory only",
     .committed = false,
     .manager_visits = true},
    {.fp = failpoint::fp3,
     .name = "FP3",
     .at = "mid commit frame write",
     .outcome = "not committed; torn tail truncated on recovery",
     .committed = false,
     .manager_visits = false},
    {.fp = failpoint::fp4,
     .name = "FP4",
     .at = "after write, before fsync",
     .outcome = "either, decided by frame intactness; no ack was given, retry by idempotency key",
     .committed = false,
     .manager_visits = false},
    {.fp = failpoint::fp5,
     .name = "FP5",
     .at = "after fsync, before ack",
     .outcome = "committed; retry returns the original receipt",
     .committed = true,
     .manager_visits = true},
    {.fp = failpoint::fp6,
     .name = "FP6",
     .at = "after ack, before events",
     .outcome = "committed; recovery emits tx recovered",
     .committed = true,
     .manager_visits = true},
}};

std::size_t index_of(failpoint fp) noexcept {
  return static_cast<std::size_t>(fp) % failpoint_count;
}

} // namespace

std::span<const failpoint_info> failpoint_table() noexcept { return table; }

std::string_view to_string(failpoint fp) noexcept {
  const auto n = static_cast<std::size_t>(fp);
  return n < failpoint_count ? std::span{table}[n].name : std::string_view{"FP?"};
}

result<void> failpoint_set::visit(failpoint fp) noexcept {
  const std::size_t i = index_of(fp);
  std::span{visits_}[i].fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t code = std::span{armed_}[i].exchange(0, std::memory_order_acq_rel);
  if (code == 0) {
    return {};
  }
  std::span{trips_}[i].fetch_add(1, std::memory_order_relaxed);
  const failpoint_info &info = std::span{table}[i];
  return make_error(static_cast<errc>(code), std::string("failpoint ") + std::string(info.name) +
                                                 " (" + std::string(info.at) + ") tripped");
}

std::uint64_t failpoint_set::visits(failpoint fp) const noexcept {
  return std::span{visits_}[index_of(fp)].load(std::memory_order_relaxed);
}

std::uint64_t failpoint_set::trips(failpoint fp) const noexcept {
  return std::span{trips_}[index_of(fp)].load(std::memory_order_relaxed);
}

bool failpoint_set::armed(failpoint fp) const noexcept {
  return std::span{armed_}[index_of(fp)].load(std::memory_order_acquire) != 0;
}

} // namespace sl::fabric
