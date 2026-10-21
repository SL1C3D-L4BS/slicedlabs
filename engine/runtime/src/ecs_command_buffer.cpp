// ECS command buffers: recording (WO-091, SL-ARCH-003 ARCH3-R118, `limits`
// command_buffer_block_bytes). Commands and payloads are carved from 16384-byte blocks of the
// job's frame arena; blocks are never released (the arena reclaims them at its flip).

#include "sl/runtime/ecs/command_buffer.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

namespace sl::runtime::ecs {

namespace {

[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t align) noexcept {
  return (value + align - 1) & ~(align - 1);
}

} // namespace

void *command_buffer::take(std::size_t bytes, std::size_t align) noexcept {
  // Every piece is a command header, an id or pointer array of at most 32 entries or one
  // component value (at most 2048 bytes, 64-byte aligned): it always fits one block.
  const bool fits = std::has_single_bit(align) && align <= command_buffer_block_alignment &&
                    bytes <= command_buffer_block_bytes;
  SL_ASSERT(fits, "command_buffer::take: a piece larger than a block or over-aligned");
  if (fault_.fault != errc::ok) {
    return nullptr;
  }
  const std::size_t need = std::max<std::size_t>(bytes, 1);
  if (block_ != nullptr) {
    const std::size_t at = align_up(used_, align);
    if (at + need <= command_buffer_block_bytes) {
      used_ = at + need;
      return block_ + at;
    }
  }
  auto block = memory_->try_allocate(command_buffer_block_bytes, command_buffer_block_alignment,
                                     memory_->default_tag());
  if (!block) {
    fail(block.error().code);
    return nullptr;
  }
  block_ = static_cast<std::byte *>(*block);
  used_ = need;
  return block_;
}

detail::command *command_buffer::start(op_kind kind, const entity_ref &target) noexcept {
  void *p = take(sizeof(detail::command), alignof(detail::command));
  if (p == nullptr) {
    return nullptr;
  }
  auto *c = ::new (p) detail::command{};
  c->kind = kind;
  c->target = target;
  c->buffer = id_;
  c->seq = size_;
  return c;
}

void command_buffer::append(detail::command *c) noexcept {
  if (last_ == nullptr) {
    first_ = c;
  } else {
    last_->next = c;
  }
  last_ = c;
  ++size_;
}

pending_entity command_buffer::spawn() noexcept {
  const pending_entity handle{.buffer = id_, .seq = size_};
  if (detail::command *c = start(op_kind::spawn, entity_ref{}); c != nullptr) {
    append(c);
  }
  return handle;
}

void command_buffer::destroy(const entity_ref &target) noexcept {
  if (detail::command *c = start(op_kind::destroy, target); c != nullptr) {
    append(c);
  }
}

void command_buffer::remove_id(const entity_ref &target, component_id component) noexcept {
  if (detail::command *c = start(op_kind::remove, target); c != nullptr) {
    c->component = component;
    append(c);
  }
}

void command_buffer::fail(errc code) noexcept {
  if (fault_.fault == errc::ok) {
    fault_.fault = code == errc::ok ? errc::internal : code;
  }
}

void command_buffer::reset(const det::ordered_key &base, std::uint32_t index,
                           mem::resource &memory) noexcept {
  memory_ = &memory;
  base_ = base;
  id_ = index;
  block_ = nullptr;
  used_ = 0;
  first_ = nullptr;
  last_ = nullptr;
  size_ = 0;
  fault_ = detail::command{};
  fault_.buffer = index;
  index_.clear();
  if (base.local_seq != 0) {
    fail(errc::invalid_argument);
  }
}

void command_buffer::build_index() noexcept {
  index_.clear();
  index_.reserve(std::size_t{size_} + 1);
  det::ordered_key k = base_;
  for (detail::command *c = first_; c != nullptr; c = c->next) {
    k.local_seq = c->seq;
    index_.push(k, c);
  }
  if (fault_.fault != errc::ok) {
    // The failure marker: playback stops here, after every command of a smaller key.
    fault_.seq = size_;
    k.local_seq = size_;
    index_.push(k, &fault_);
  }
}

void command_buffer::release() noexcept {
  for (const detail::command *c = first_; c != nullptr; c = c->next) {
    for (std::uint16_t i = 0; i < c->count; ++i) {
      registry_->info(c->ids[i]).destroy(c->values[i]);
    }
  }
  block_ = nullptr;
  used_ = 0;
  first_ = nullptr;
  last_ = nullptr;
  size_ = 0;
  fault_ = detail::command{};
  index_.clear();
}

} // namespace sl::runtime::ecs
