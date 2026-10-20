// The job record pool (WO-082, SL-ARCH-003 ARCH3-R062, R063, `limits` job_record_bytes,
// job_max_dependencies). capacity records of 128 bytes aligned to 64 and their cold side data are
// allocated once at start; free records circulate through a FIFO ring under free_mutex_, so a
// finished record keeps its status for as long as possible before reuse. A record's generation
// starts at 1 and advances at every allocation (0 is skipped); a handle whose generation is not
// the record's is stale. Dependents: the first four sit in the record's fixed array, the rest in
// an overflow chain whose links live in the dependent's own cold data (one link per dependency
// slot, so the pool of links is capacity x 8 and never runs out). A record's status change to a
// final value and its dependents list are guarded by one of 64 stripe mutexes (index % 64).
// Every atomic operation is seq_cst (no explicit memory order).

#include "sl/runtime/error.hpp"
#include "sl/runtime/jobs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>

namespace sl::runtime::jobs {

namespace {

// A link names dependency slot `slot` (0-7) of record `dependent`.
constexpr std::uint32_t make_link(std::uint32_t dependent, std::uint32_t slot) noexcept {
  return (dependent * static_cast<std::uint32_t>(job_max_dependencies)) + slot;
}

constexpr bool is_final(job_status s) noexcept {
  return s == job_status::done || s == job_status::failed || s == job_status::cancelled;
}

} // namespace

result<void> job_system::init_records() noexcept {
  records_.reset(new (std::nothrow) record[capacity_]);
  cold_.reset(new (std::nothrow) record_cold[capacity_]);
  free_ring_.reset(new (std::nothrow) std::uint32_t[capacity_]);
  if (!records_ || !cold_ || !free_ring_) {
    return make_error(errc::out_of_memory, "job_system::start: record pool allocation failed");
  }
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    records_[i].index = i;
    free_ring_[i] = i;
  }
  free_head_ = 0;
  free_tail_ = capacity_;
  return {};
}

std::optional<std::uint32_t> job_system::allocate_record() noexcept {
  std::uint32_t index = 0;
  {
    const std::scoped_lock lock(free_mutex_);
    if (free_head_ == free_tail_) {
      return std::nullopt;
    }
    index = free_ring_[free_head_ % capacity_];
    ++free_head_;
  }
  record &r = records_[index];
  const std::scoped_lock lock(stripe(index));
  std::uint32_t generation = r.generation.load() + 1U;
  if (generation == 0) {
    generation = 1;
  }
  r.generation.store(generation);
  r.status.store(job_status::pending);
  r.dependents = 0;
  r.overflow = no_link;
  return index;
}

void job_system::free_record(std::uint32_t index) noexcept {
  const std::scoped_lock lock(free_mutex_);
  free_ring_[free_tail_ % capacity_] = index;
  ++free_tail_;
}

job_system::attach_result job_system::attach(job_handle pred, std::uint32_t dependent,
                                             std::uint32_t slot) noexcept {
  if (pred.generation == 0) {
    return attach_result::satisfied; // the null handle: a job run inline or never submitted
  }
  record &p = records_[pred.index];
  const std::scoped_lock lock(stripe(pred.index));
  if (p.generation.load() != pred.generation) {
    return attach_result::satisfied; // stale: finished long ago, its outcome is gone
  }
  const job_status s = p.status.load();
  if (s == job_status::done) {
    return attach_result::satisfied;
  }
  if (is_final(s)) {
    return attach_result::cancel;
  }
  records_[dependent].remaining.fetch_add(1);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): dependents < 4 and slot < 8 are
  // checked
  if (p.dependents < fixed_dependents) {
    p.fixed[p.dependents] = make_link(dependent, slot);
    ++p.dependents;
  } else {
    cold_[dependent].next[slot] = p.overflow;
    p.overflow = make_link(dependent, slot);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  return attach_result::attached;
}

void job_system::release_dependents(std::uint32_t index, job_status final_status,
                                    thread_slot *slot) noexcept {
  record &r = records_[index];
  std::array<std::uint32_t, fixed_dependents> fixed{};
  std::size_t count = 0;
  std::uint32_t chain = no_link;
  {
    const std::scoped_lock lock(stripe(index));
    r.status.store(final_status);
    count = r.dependents;
    fixed = r.fixed;
    chain = r.overflow;
    r.dependents = 0;
    r.overflow = no_link;
  }
  const bool cancel = final_status != job_status::done;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): count <= 4, a link's slot is <
  // 8
  for (std::size_t i = 0; i < count; ++i) {
    release_one(fixed[i] / job_max_dependencies, cancel, slot);
  }
  while (chain != no_link) {
    const std::uint32_t dependent = chain / job_max_dependencies;
    // Read the next link before the release: once its count reaches zero the dependent may run,
    // finish and be reused.
    const std::uint32_t next = cold_[dependent].next[chain % job_max_dependencies];
    release_one(dependent, cancel, slot);
    chain = next;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
}

std::string_view job_system::record_name(std::uint64_t packed) const noexcept {
  const auto index = static_cast<std::uint32_t>(packed & 0xFFFFFFFFU);
  const auto generation = static_cast<std::uint32_t>(packed >> 32U);
  if (generation == 0 || index >= capacity_) {
    return "-";
  }
  const record &r = records_[index];
  if (r.generation.load() != generation) {
    return "(finished)";
  }
  const char *ptr = r.name_ptr.load();
  const std::uint16_t len = r.name_len.load();
  // A reuse stores the new generation before the new name, so an unchanged generation after
  // the reads means the name belongs to this handle.
  if (r.generation.load() != generation) {
    return "(finished)";
  }
  if (ptr == nullptr) {
    return "(unnamed)";
  }
  return {ptr, len};
}

} // namespace sl::runtime::jobs
