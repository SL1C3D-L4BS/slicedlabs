// World resources (WO-088, SL-ARCH-003 ARCH3-R109, `determinism` world resources, ADR-0025): one
// value per resource_key, kept in ascending key order, each in its own block from the world's
// resource. add_resource is owner-thread; resource() is a read (worker-only inside systems).

#include "sl/runtime/ecs/world.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <thread>

namespace sl::runtime::ecs {

namespace {

[[nodiscard]] std::size_t lower(std::span<const detail::resource_slot> slots,
                                resource_key key) noexcept {
  const auto it = std::ranges::lower_bound(slots, key, std::less<>{}, &detail::resource_slot::key);
  return static_cast<std::size_t>(it - slots.begin());
}

} // namespace

result<void> world::add_resource_raw(const detail::resource_slot &slot, void *value) noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "world::add_resource off the owner thread");
  }
  check_access();
  if (slot.move == nullptr || slot.destroy == nullptr || slot.size == 0 ||
      !mem::is_valid_alignment(slot.align)) {
    return make_error(errc::invalid_argument, "world::add_resource: malformed resource slot");
  }
  const std::size_t at = lower(resources_.span(), slot.key);
  if (at < resources_.size() && resources_[at].key == slot.key) {
    return make_error(errc::already_exists,
                      "world::add_resource: key " + std::to_string(slot.key.value) + " is used");
  }
  if (auto ok = resources_.reserve(resources_.size() + 1); !ok) {
    return ok;
  }
  auto block = memory_->try_allocate(slot.size, slot.align, meta_tag_);
  if (!block) {
    return std::unexpected(block.error());
  }
  detail::resource_slot stored = slot;
  stored.data = *block;
  stored.move(stored.data, value);
  resources_.insert_at(at, stored);
  return {};
}

result<void *> world::resource_raw(resource_key key, const void *type_key) const noexcept {
  check_access();
  const std::size_t at = lower(resources_.span(), key);
  if (at == resources_.size() || resources_[at].key != key) {
    return make_error(errc::not_found,
                      "world::resource: no resource under key " + std::to_string(key.value));
  }
  if (resources_[at].type_key != type_key) {
    return make_error(errc::invalid_argument,
                      "world::resource: key " + std::to_string(key.value) + " holds another type");
  }
  return resources_[at].data;
}

void world::destroy_resources() noexcept {
  for (const detail::resource_slot &s : resources_.span()) {
    s.destroy(s.data);
    memory_->release(s.data, s.size, s.align);
  }
  resources_.clear();
}

} // namespace sl::runtime::ecs
