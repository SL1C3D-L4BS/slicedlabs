// component_registry (WO-088, SL-ARCH-003 ARCH3-R106, R113, `limits` max_component_types,
// max_component_size, max_component_alignment, ADR-0025). Ids are registration indices; names are
// copied into strings reserved up front so the views in component_info stay valid. The layout
// digest hashes, per component in id order, the name (str), size (u32), alignment (u32) and the
// field layout: a u32 field count, 0 until the reflected field walk joins it (WO-088.md).

#include "sl/runtime/ecs/component.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace sl::runtime::ecs {

component_registry::component_registry() {
  names_.reserve(max_component_types);
  infos_.reserve(max_component_types);
}

result<component_id> component_registry::add_raw(const component_info &desc) noexcept {
  if (frozen_) {
    return make_error(errc::component_registry_frozen,
                      "component_registry::add after freeze: " + std::string(desc.name));
  }
  if (desc.name.empty()) {
    return make_error(errc::invalid_argument, "component_registry::add: empty name");
  }
  if (desc.size == 0 || desc.size > max_component_size) {
    return make_error(errc::component_invalid, "component " + std::string(desc.name) + ": size " +
                                                   std::to_string(desc.size) +
                                                   " outside 1..2048 bytes");
  }
  if (!std::has_single_bit(desc.align) || desc.align > max_component_alignment) {
    return make_error(errc::component_invalid, "component " + std::string(desc.name) +
                                                   ": alignment " + std::to_string(desc.align) +
                                                   " is not a power of two up to 64");
  }
  if (desc.move == nullptr || desc.destroy == nullptr) {
    return make_error(errc::component_invalid,
                      "component " + std::string(desc.name) + ": no move or destroy function");
  }
  if (count_ >= max_component_types) {
    return make_error(errc::component_invalid, "component_registry: more than 1024 types");
  }
  if (std::ranges::find(names_, desc.name) != names_.end()) {
    return make_error(errc::already_exists,
                      "component " + std::string(desc.name) + " is already registered");
  }
  if (desc.type_key != nullptr && by_key_.contains(desc.type_key)) {
    return make_error(errc::already_exists,
                      "component " + std::string(desc.name) + ": its type is already registered");
  }
  names_.emplace_back(desc.name);
  component_info stored = desc;
  stored.name = names_.back();
  infos_.push_back(stored);
  const component_id id{count_};
  if (desc.type_key != nullptr) {
    by_key_.emplace(desc.type_key, count_);
  }
  ++count_;
  return id;
}

result<component_id> component_registry::find(std::string_view name) const noexcept {
  const auto it = std::ranges::find(names_, name);
  if (it != names_.end()) {
    return component_id{static_cast<std::uint16_t>(it - names_.begin())};
  }
  return make_error(errc::component_unknown, "component " + std::string(name) + " is unknown");
}

result<component_id> component_registry::find_key(const void *type_key) const noexcept {
  const auto it = by_key_.find(type_key);
  if (it == by_key_.end()) {
    return make_error(errc::component_unknown, "component type is not registered");
  }
  return component_id{it->second};
}

const component_info &component_registry::info(component_id id) const noexcept {
  SL_ASSERT(id.value < count_, "component_registry::info of an unregistered id");
  return infos_[id.value];
}

result<void> component_registry::freeze() noexcept {
  frozen_ = true;
  return {};
}

fabric::digest component_registry::layout_digest() const noexcept {
  det::state_hasher h;
  h.str("sl-ecs-layout/1");
  h.u32(count_);
  for (std::uint16_t i = 0; i < count_; ++i) {
    const component_info &c = infos_[i];
    h.str(c.name);
    h.u32(c.size);
    h.u32(c.align);
    h.u32(0); // the reflected field layout: no fields walked yet (WO-088.md)
  }
  return h.finish();
}

} // namespace sl::runtime::ecs
