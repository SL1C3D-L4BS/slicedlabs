// tick_scope (WO-084, SL-ARCH-003 ARCH3-R083, master plan section 3 F6): the calling thread's
// sl::log correlation becomes {id, runtime/<instance>, tick} for the scope's lifetime; the id is
// kept from an enclosing correlation when it has one, so a frame's tick scopes share the frame's
// id, and drawn fresh otherwise. Correlation is diagnostic only (ARCH3-R068).

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/time.hpp"

#include <cstdint>
#include <string_view>

namespace sl::runtime::time {

namespace {

log::correlation tick_correlation(tick t, std::string_view scope) noexcept {
  SL_DEBUG_ASSERT(scope.starts_with("runtime/"), "tick_scope: the scope is runtime/<instance>");
  const std::uint64_t current = log::current_correlation().id;
  return log::correlation{
      .id = current != 0 ? current : log::next_correlation_id(), .scope = scope, .tick = t};
}

} // namespace

tick_scope::tick_scope(tick t, std::string_view scope) noexcept
    : scope_(tick_correlation(t, scope)) {}

} // namespace sl::runtime::time
