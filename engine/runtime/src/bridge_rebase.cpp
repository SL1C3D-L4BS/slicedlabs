// post_projection and the projection_rebase slot of the link (WO-097, SL-ARCH-003 ARCH3-R132,
// R157, R158; ADR-0027, ADR-0023).

#include "sl/runtime/bridge.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/reflect.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace sl::runtime::link {

namespace fab = sl::fabric;

result<void> world_link::post_projection(fab::document_projection projection) noexcept {
  detail::link_state &s = *state_;
  if (!detail::same_domains(s, projection)) {
    return make_error(errc::invalid_argument, "world_link::post_projection: other domains");
  }
  const std::uint64_t bytes = projection_bytes(projection);
  const std::scoped_lock lock(s.mutex);
  if (s.has_current && projection.generation() <= s.current_generation) {
    return make_error(errc::projection_stale,
                      "world_link::post_projection: the generation is not above the current one");
  }
  s.pending = std::move(projection);
  s.pending_bytes = bytes;
  return {};
}

namespace detail {

namespace {

// Rewrites the bound fields of `e` from `body` in place: the runtime-only bytes stay, no changed
// tick is stamped (write_source::document, ARCH3-R132).
bool update_record(link_state &s, ecs::entity e, const binding_registry::entry &b,
                   const fab::record_id &record, const fab::value &body) noexcept {
  const std::uint32_t size = s.world->registry().info(b.component).size;
  std::ranges::fill(s.scratch.bytes, std::byte{0});
  if (auto now = s.world->get_raw(e, b.component)) {
    std::memcpy(s.scratch.bytes.data(), *now, size);
  }
  if (!b.binding.from_value(body, s.scratch.bytes.data())) {
    return false;
  }
  if (!s.world->set_raw(e, b.component, s.scratch.bytes.data(), ecs::write_source::document)) {
    return false;
  }
  cache_record(s, record, b.binding, body);
  return true;
}

void apply_diff(link_state &s, const fab::document_projection &next, std::uint64_t tick) noexcept {
  // Retired: mapped records the new projection no longer holds.
  std::vector<fab::record_id> retired;
  s.entities.for_each([&](const fab::record_id &id, ecs::entity /*e*/) {
    if (!next.check_scope(id)) {
      retired.push_back(id);
    }
  });
  for (const fab::record_id &id : retired) {
    if (auto e = s.entities.find(id)) {
      const result<void> destroyed = s.world->destroy(*e);
      SL_DEBUG_ASSERT(destroyed.has_value(), "link rebase: a mapped entity is alive");
    }
    const result<void> erased = s.entities.erase(id);
    SL_DEBUG_ASSERT(erased.has_value(), "link rebase: the retired record is mapped");
    s.projected.erase(id);
    ++s.stats.rebase_retired;
  }
  // Changed and created, in load_plan order.
  auto source = document_source::make(next);
  if (!source) {
    return;
  }
  std::vector<load_entry> created;
  for (const load_entry &entry : source->load_plan()) {
    const binding_registry::entry *b = s.bindings->entry_of(entry.type);
    if (b == nullptr) {
      ++s.stats.skipped_unbound;
      continue;
    }
    auto e = s.entities.find(entry.record);
    if (!e) {
      created.push_back(entry);
      continue;
    }
    auto now = next.get(entry.record);
    if (!now || now->body == nullptr) {
      continue;
    }
    std::optional<fab::record_view> was;
    if (s.current) {
      if (auto v = s.current->get(entry.record)) {
        was = *v;
      }
    }
    if (was && was->body != nullptr && *was->body == *now->body) {
      continue; // unchanged: the handle and the value stay
    }
    if (update_record(s, *e, *b, entry.record, *now->body)) {
      ++s.stats.rebase_updated;
    }
  }
  if (auto spawned = spawn_entries(s, created, next, tick)) {
    s.stats.rebase_created += *spawned;
  }
}

} // namespace

void report_projection_bytes(link_state &s) noexcept {
  std::uint64_t pending = 0;
  {
    const std::scoped_lock lock(s.mutex);
    pending = s.pending_bytes;
  }
  const std::uint64_t held = (s.current ? s.current_bytes : 0) + pending;
  s.stats.projection_bytes = held;
  // attach reported on the tag already, so it is reserved as external.
  const result<void> reported = s.diagnostics->report_external(s.config.projection_tag, held);
  SL_DEBUG_ASSERT(reported.has_value(), "link: fabric.projection is reserved as external");
}

void rebase_projection(link_state &s, std::uint64_t tick) noexcept {
  check_alive(s);
  std::optional<fab::document_projection> next;
  std::uint64_t next_bytes = 0;
  {
    const std::scoped_lock lock(s.mutex);
    next = std::move(s.pending);
    s.pending.reset();
    next_bytes = s.pending_bytes;
    s.pending_bytes = 0;
  }
  if (next) {
    // Keyed by (root_digest, generation): equal document state needs no diff.
    if (!s.current || s.current->root_digest() != next->root_digest()) {
      apply_diff(s, *next, tick);
    }
    s.current = std::move(next); // the old projection is released here
    s.current_bytes = next_bytes;
    const std::scoped_lock lock(s.mutex);
    s.current_generation = s.current->generation();
    s.has_current = true;
  }
  report_projection_bytes(s);
}

} // namespace detail

} // namespace sl::runtime::link
