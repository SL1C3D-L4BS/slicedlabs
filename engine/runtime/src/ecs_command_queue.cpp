// ECS command buffers: open, playback and clear (WO-091, SL-ARCH-003 ARCH3-R119, R120, R086,
// `determinism` merge point "command buffer playback"). flush merges every buffer through
// det::merge_ordered by (base key, local_seq) and applies each command through world's public
// structural API on the owner thread.

#include "sl/runtime/ecs/command_buffer.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <variant>

namespace sl::runtime::ecs {

/// The merge_ordered consumer of flush: applies one command, or stops at the first error.
struct command_player {
  using op_kind = flush_report::op_kind;

  const command_queue *queue = nullptr;
  world *w = nullptr;
  flush_report *report = nullptr;
  std::optional<sl::error> stop;

  void operator()(const det::ordered_key & /*key*/, detail::command *c) noexcept {
    if (stop.has_value()) {
      return;
    }
    if (c->fault != errc::ok) {
      stop = make_error(c->fault, "command_buffer: recording failed").error();
      return;
    }
    if (c->kind == op_kind::spawn) {
      spawn(*c);
      return;
    }
    entity target;
    if (!queue->resolve(c->target, target)) {
      ++report->skipped_stale;
      return;
    }
    result<void> done;
    component_id component = c->component;
    switch (c->kind) {
    case op_kind::destroy:
      done = w->destroy(target);
      component = component_id{};
      break;
    case op_kind::add:
      done = w->add_raw(target, c->component, c->value, write_source::runtime);
      break;
    case op_kind::remove:
      done = w->remove_raw(target, c->component);
      break;
    case op_kind::set:
      done = w->set_raw(target, c->component, c->value, write_source::runtime);
      break;
    case op_kind::spawn:
      break;
    }
    if (done) {
      report->applied.push_back(
          flush_report::op{.kind = c->kind, .target = target, .component = component});
      return;
    }
    const errc code = done.error().code;
    if (code == errc::stale_entity) {
      ++report->skipped_stale;
    } else if (code == errc::component_exists || code == errc::component_missing) {
      ++report->skipped_refused;
    } else {
      stop = std::move(done.error());
    }
  }

  void spawn(detail::command &c) noexcept {
    auto e = w->create_raw(std::span<const component_id>(c.ids, c.count),
                           std::span<void *const>(c.values, c.count));
    if (!e) {
      stop = std::move(e.error());
      return;
    }
    c.resolved = *e;
    report->resolved.emplace_back(pending_entity{.buffer = c.buffer, .seq = c.seq}, *e);
    report->applied.push_back(
        flush_report::op{.kind = op_kind::spawn, .target = *e, .component = component_id{}});
  }
};

command_queue::command_queue(const component_registry &frozen, mem::frame_arena &arena) noexcept
    : registry_(&frozen), arena_(&arena), owner_(std::this_thread::get_id()) {}

command_queue::~command_queue() { release(open_); }

command_buffer &command_queue::open(const det::ordered_key &base) noexcept {
  return open(base, arena_->workers());
}

command_buffer &command_queue::open(const det::ordered_key &base, std::uint32_t worker) noexcept {
  mem::resource &memory = arena_->for_worker(worker);
  const std::scoped_lock lock(mutex_);
  if (open_ == pool_.size()) {
    pool_.push_back(std::make_unique<command_buffer>(command_buffer::passkey{}, *registry_));
  }
  command_buffer &b = *pool_[open_];
  b.reset(base, open_, memory);
  ++open_;
  return b;
}

std::uint32_t command_queue::open_count() const noexcept {
  const std::scoped_lock lock(mutex_);
  return open_;
}

bool command_queue::resolve(const entity_ref &target, entity &out) const noexcept {
  if (const entity *e = std::get_if<entity>(&target); e != nullptr) {
    out = *e;
    return true;
  }
  const pending_entity *p = std::get_if<pending_entity>(&target);
  if (p == nullptr || p->buffer >= merge_.size()) {
    return false;
  }
  const auto entries = std::as_const(*merge_[p->buffer]).entries();
  if (p->seq >= entries.size()) {
    return false;
  }
  const detail::command *c = entries[p->seq].value;
  if (c->fault != errc::ok || c->kind != flush_report::op_kind::spawn || c->resolved.is_null()) {
    return false;
  }
  out = c->resolved;
  return true;
}

bool command_queue::duplicate_keys(std::uint32_t buffers) noexcept {
  keys_.clear();
  for (std::uint32_t i = 0; i < buffers; ++i) {
    keys_.push_back(pool_[i]->key());
  }
  std::ranges::sort(keys_);
  return std::ranges::adjacent_find(keys_) != keys_.end();
}

void command_queue::release(std::uint32_t buffers) noexcept {
  for (std::uint32_t i = 0; i < buffers; ++i) {
    pool_[i]->release();
  }
  merge_.clear();
  const std::scoped_lock lock(mutex_);
  open_ = 0;
}

result<flush_report> command_queue::flush(world &w) noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "command_queue::flush off the owner thread");
  }
  if (&w.registry() != registry_) {
    return make_error(errc::invalid_argument, "command_queue::flush: a world of another registry");
  }
  const std::uint32_t buffers = open_count();
  if (duplicate_keys(buffers)) {
    release(buffers);
    return make_error(errc::invalid_argument, "command_queue::flush: two buffers share a base key");
  }
  merge_.clear();
  std::size_t commands = 0;
  for (std::uint32_t i = 0; i < buffers; ++i) {
    command_buffer &b = *pool_[i];
    b.build_index();
    merge_.push_back(&b.index_);
    commands += b.size();
  }
  flush_report report;
  report.applied.reserve(commands);
  command_player player{.queue = this, .w = &w, .report = &report, .stop = std::nullopt};
  const bool was_active = w.phase_active();
  w.set_phase_active(false);
  const std::span<det::ordered_buffer<detail::command *> *const> in(merge_);
  det::merge_ordered(in, player); // neg-completion-order
  w.set_phase_active(was_active);
  release(buffers);
  if (player.stop.has_value()) {
    return std::unexpected(std::move(*player.stop));
  }
  return report;
}

void command_queue::clear() noexcept {
  SL_DEBUG_ASSERT(std::this_thread::get_id() == owner_,
                  "command_queue::clear off the owner thread");
  if (std::this_thread::get_id() != owner_) {
    return;
  }
  release(open_count());
}

} // namespace sl::runtime::ecs
