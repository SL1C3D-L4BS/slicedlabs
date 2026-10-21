// The Debug access guard (WO-094, SL-ARCH-003 ARCH3-R125): with NDEBUG undefined, query
// iteration, system_context::resource and system_context::commands check the system's declared
// access_set; a violation fails the system's job with undeclared_access (job_context::fail),
// counts a failure (the frame's command buffers are discarded at the phase boundary) and stops
// the system. An exclusive system may touch anything. Release builds compile the checks out.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/jobs.hpp"

#include <algorithm>
#include <vector>

namespace sl::runtime::ecs {

namespace detail {

bool covers(const component_access &declared, const component_access &needed) noexcept {
  const auto declared_write = [&declared](component_id c) noexcept {
    return std::ranges::binary_search(declared.writes, c);
  };
  const auto declared_any = [&](component_id c) noexcept {
    return declared_write(c) || std::ranges::binary_search(declared.reads, c);
  };
  return std::ranges::all_of(needed.reads, declared_any) &&
         std::ranges::all_of(needed.writes, declared_write);
}

} // namespace detail

bool system_context::allow_query(const component_access &needed) noexcept {
  if (stopped_) {
    return false;
  }
  if constexpr (access_guard_enabled) {
    if (!record_->access.exclusive && !detail::covers(record_->access.components, needed)) {
      violation();
      return false;
    }
  }
  return true;
}

bool system_context::allow_resource(resource_key key, bool write) noexcept {
  if constexpr (access_guard_enabled) {
    const access_set &a = record_->access;
    if (a.exclusive) {
      return true;
    }
    const bool declared = std::ranges::binary_search(a.resource_writes, key) ||
                          (!write && std::ranges::binary_search(a.resource_reads, key));
    if (!declared) {
      violation();
      return false;
    }
  } else {
    static_cast<void>(key);
    static_cast<void>(write);
  }
  return true;
}

void system_context::check_commands() noexcept {
  if constexpr (access_guard_enabled) {
    if (!record_->access.structural && !record_->access.exclusive) {
      violation();
    }
  }
}

void system_context::violation() noexcept {
  if (stopped_) {
    return;
  }
  job.fail(errc::undeclared_access);
  scheduler_->note_failure(*record_, errc::undeclared_access);
  stopped_ = true;
}

} // namespace sl::runtime::ecs
