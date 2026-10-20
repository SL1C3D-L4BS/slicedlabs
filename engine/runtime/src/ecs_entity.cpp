// entity_allocator (WO-083, SL-ARCH-003 ARCH3-R101 to R104, `failpoints` RFP10, ADR-0025). The
// entity table is paged: each page holds entity_page_records generations, the free-FIFO link of
// each record and an alive bit per record, in one block from the allocator's resource that never
// moves. The free FIFO is intrusive (linked through the pages), so destroy never allocates. Pages
// are allocated in ascending order, only by create (and restore), on the owner thread.

#include "sl/runtime/ecs/entity.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sl::runtime::ecs {

namespace {

constexpr std::uint32_t alive_words = entity_page_records / 64;
constexpr std::size_t page_alignment = 64;

[[nodiscard]] constexpr std::uint32_t slot_of(std::uint32_t index) noexcept {
  return index % entity_page_records;
}

[[nodiscard]] constexpr std::uint32_t page_index_of(std::uint32_t index) noexcept {
  return index / entity_page_records;
}

[[nodiscard]] constexpr std::uint64_t alive_mask(std::uint32_t slot) noexcept {
  return std::uint64_t{1} << (slot % 64U);
}

[[nodiscard]] std::string describe(entity e) {
  return "{" + std::to_string(e.index) + ", " + std::to_string(e.generation) + "}";
}

} // namespace

// Slots are below entity_page_records by construction (slot_of), so the accessors index through
// data() without a bounds check.
struct entity_allocator::page {
  std::array<std::uint32_t, entity_page_records> generations{};
  std::array<std::uint32_t, entity_page_records> links{};
  std::array<std::uint64_t, alive_words> alive_bits{};

  // A page as fresh: every generation entity_generation_first, nothing alive, no links.
  void reset() noexcept {
    generations.fill(entity_generation_first);
    links.fill(no_index);
    alive_bits.fill(0);
  }
  [[nodiscard]] std::uint32_t &generation(std::uint32_t slot) noexcept {
    return *(generations.data() + slot);
  }
  [[nodiscard]] std::uint32_t generation(std::uint32_t slot) const noexcept {
    return *(generations.data() + slot);
  }
  [[nodiscard]] std::uint32_t &next_free(std::uint32_t slot) noexcept {
    return *(links.data() + slot);
  }
  [[nodiscard]] bool is_alive(std::uint32_t slot) const noexcept {
    return (*(alive_bits.data() + (slot / 64U)) & alive_mask(slot)) != 0;
  }
  void set_alive(std::uint32_t slot, bool on) noexcept {
    std::uint64_t &word = *(alive_bits.data() + (slot / 64U));
    if (on) {
      word |= alive_mask(slot);
    } else {
      word &= ~alive_mask(slot);
    }
  }
};

entity_allocator::entity_allocator(std::uint32_t max_entities, mem::resource &memory)
    : memory_(&memory), max_entities_(max_entities),
      pages_((std::size_t{max_entities} + entity_page_records - 1) / entity_page_records, nullptr,
             &memory) {}

entity_allocator::~entity_allocator() {
  for (std::uint32_t i = 0; i < page_count_; ++i) {
    release_page(pages_[i]);
    pages_[i] = nullptr;
  }
}

entity_allocator::page *entity_allocator::page_of(std::uint32_t index) const noexcept {
  const std::uint32_t p = page_index_of(index);
  return p < page_count_ ? pages_[p] : nullptr;
}

result<entity_allocator::page *> entity_allocator::allocate_page() noexcept {
  auto block = memory_->try_allocate(sizeof(page), page_alignment, memory_->default_tag());
  if (!block) {
    return std::unexpected(block.error());
  }
  page *fresh = std::construct_at(static_cast<page *>(*block));
  fresh->reset();
  return fresh;
}

void entity_allocator::release_page(page *p) noexcept {
  std::destroy_at(p);
  memory_->release(p, sizeof(page), page_alignment);
}

const std::uint32_t *entity_allocator::page_address(std::uint32_t page_number) const noexcept {
  return page_number < page_count_ ? pages_[page_number]->generations.data() : nullptr;
}

void entity_allocator::push_free(std::uint32_t index) noexcept {
  page_of(index)->next_free(slot_of(index)) = no_index;
  if (free_tail_ == no_index) {
    free_head_ = index;
  } else {
    page_of(free_tail_)->next_free(slot_of(free_tail_)) = index;
  }
  free_tail_ = index;
  ++free_count_;
}

std::uint32_t entity_allocator::pop_free() noexcept {
  const std::uint32_t index = free_head_;
  page *p = page_of(index);
  free_head_ = p->next_free(slot_of(index));
  p->next_free(slot_of(index)) = no_index;
  if (free_head_ == no_index) {
    free_tail_ = no_index;
  }
  --free_count_;
  return index;
}

result<entity> entity_allocator::create() noexcept {
  bool saturate = false;
#if SL_RUNTIME_FAULTS
  const fault_action fault = visit(failpoint::rfp10);
  if (fault.fire) {
    if (fault.param != 1) {
      return make_error(errc::entity_capacity, "RFP10: entity capacity forced");
    }
    saturate = true;
  }
#endif
  // A fresh index while fewer than the threshold are free and max_entities is not reached; else
  // the oldest free index; else capacity.
  std::uint32_t index = 0;
  if (free_count_ < entity_reuse_fifo_threshold && next_index_ < max_entities_) {
    if (page_index_of(next_index_) == page_count_) {
      auto fresh = allocate_page();
      if (!fresh) {
        return std::unexpected(fresh.error());
      }
      pages_[page_count_] = *fresh;
      ++page_count_;
    }
    index = next_index_;
    ++next_index_;
  } else if (free_count_ > 0) {
    index = pop_free();
  } else {
    return make_error(errc::entity_capacity, "entity_allocator: max_entities " +
                                                 std::to_string(max_entities_) +
                                                 " reached and no free index");
  }
  page *p = page_of(index);
  const std::uint32_t slot = slot_of(index);
  if (saturate) {
    p->generation(slot) = entity_generation_retire - 1;
  }
  p->set_alive(slot, true);
  ++alive_;
  return entity{.index = index, .generation = p->generation(slot)};
}

result<void> entity_allocator::destroy(entity e) noexcept {
  if (!is_alive(e)) {
    return make_error(errc::stale_entity, "destroy of a dead or null entity " + describe(e));
  }
  page *p = page_of(e.index);
  const std::uint32_t slot = slot_of(e.index);
  p->set_alive(slot, false);
  --alive_;
  ++p->generation(slot);
  if (p->generation(slot) == entity_generation_retire) {
    ++retired_count_;
  } else {
    push_free(e.index);
  }
  return {};
}

bool entity_allocator::is_alive(entity e) const noexcept {
  if (e.generation == 0 || e.index >= next_index_) {
    return false;
  }
  const page *p = page_of(e.index);
  const std::uint32_t slot = slot_of(e.index);
  return p != nullptr && p->is_alive(slot) && p->generation(slot) == e.generation;
}

result<void> entity_allocator::check(entity e) const noexcept {
  if (!is_alive(e)) {
    return make_error(errc::stale_entity, "stale entity handle " + describe(e));
  }
  return {};
}

void entity_allocator::assume_alive(entity e) const noexcept {
  SL_DEBUG_ASSERT(is_alive(e), "unchecked access through a stale entity handle");
  static_cast<void>(e);
}

entity_allocator::state entity_allocator::capture() const {
  state s;
  s.next_index = next_index_;
  s.generations.reserve(next_index_);
  for (std::uint32_t i = 0; i < next_index_; ++i) {
    const std::uint32_t g = page_of(i)->generation(slot_of(i));
    s.generations.push_back(g);
    if (g == entity_generation_retire) {
      s.retired.push_back(i);
    }
  }
  s.free_fifo.reserve(free_count_);
  for (std::uint32_t i = free_head_; i != no_index; i = page_of(i)->next_free(slot_of(i))) {
    s.free_fifo.push_back(i);
  }
  return s;
}

result<void> entity_allocator::restore(const state &s) {
  // Validate everything before anything changes.
  if (s.next_index > max_entities_) {
    return make_error(errc::invalid_argument, "restore: next_index above max_entities");
  }
  if (s.generations.size() != s.next_index) {
    return make_error(errc::invalid_argument, "restore: generations do not match next_index");
  }
  enum class kind : std::uint8_t { alive, free, retired };
  std::vector<kind> kinds(s.next_index, kind::alive);
  std::vector<bool> listed(s.next_index, false);
  for (const std::uint32_t i : s.free_fifo) {
    if (i >= s.next_index || listed[i] || s.generations[i] == entity_generation_retire) {
      return make_error(errc::invalid_argument, "restore: bad free index " + std::to_string(i));
    }
    listed[i] = true;
    kinds[i] = kind::free;
  }
  for (const std::uint32_t i : s.retired) {
    if (i >= s.next_index || listed[i] || s.generations[i] != entity_generation_retire) {
      return make_error(errc::invalid_argument, "restore: bad retired index " + std::to_string(i));
    }
    listed[i] = true;
    kinds[i] = kind::retired;
  }
  std::uint32_t live = 0;
  for (std::uint32_t i = 0; i < s.next_index; ++i) {
    const std::uint32_t g = s.generations[i];
    if (g == 0 || (g == entity_generation_retire && kinds[i] != kind::retired)) {
      return make_error(errc::invalid_argument, "restore: bad generation at " + std::to_string(i));
    }
    if (kinds[i] == kind::alive) {
      ++live;
    }
  }
  // Allocate the missing pages; on failure release them again.
  const auto needed = static_cast<std::uint32_t>(
      (std::size_t{s.next_index} + entity_page_records - 1) / entity_page_records);
  const std::uint32_t had = page_count_;
  for (std::uint32_t p = page_count_; p < needed; ++p) {
    auto fresh = allocate_page();
    if (!fresh) {
      for (std::uint32_t q = had; q < p; ++q) {
        release_page(pages_[q]);
        pages_[q] = nullptr;
      }
      return std::unexpected(fresh.error());
    }
    pages_[p] = *fresh;
  }
  page_count_ = std::max(page_count_, needed);
  // Commit: every page in place, as fresh, then the captured state.
  for (std::uint32_t p = 0; p < page_count_; ++p) {
    pages_[p]->reset();
  }
  next_index_ = s.next_index;
  free_head_ = no_index;
  free_tail_ = no_index;
  free_count_ = 0;
  for (std::uint32_t i = 0; i < s.next_index; ++i) {
    page *p = page_of(i);
    p->generation(slot_of(i)) = s.generations[i];
    p->set_alive(slot_of(i), kinds[i] == kind::alive);
  }
  for (const std::uint32_t i : s.free_fifo) {
    push_free(i);
  }
  retired_count_ = static_cast<std::uint32_t>(s.retired.size());
  alive_ = live;
  return {};
}

} // namespace sl::runtime::ecs
