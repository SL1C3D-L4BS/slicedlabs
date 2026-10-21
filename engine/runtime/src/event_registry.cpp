// event_registry (WO-092, SL-ARCH-003 ARCH3-R137, `error_codes` event_type_unknown and
// type_registry_frozen, `thread_safety` event_registry add, freeze): each event type is a reflected
// struct whose description is copied into the registry's own type_registry, so the payload views
// never depend on the caller's strings. Ids are 1.. in registration order; names are looked up
// through an ordered map, types through the address key of ecs::detail::type_key_of.

#include "sl/runtime/events.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace sl::runtime {

namespace {

bool printable(char c) noexcept {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x21U && u <= 0x7EU;
}

bool printable_name(std::string_view name) noexcept {
  return !name.empty() && std::ranges::all_of(name, &printable);
}

} // namespace

event_registry::event_registry() noexcept : owner_(std::this_thread::get_id()) {}

result<event_type_id> event_registry::add_raw(std::string_view event_name, const type_info &info,
                                              const void *type_key, std::size_t bytes,
                                              std::size_t alignment) noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "event_registry::add off the owner thread");
  }
  if (frozen_) {
    return make_error(errc::type_registry_frozen, "event_registry::add after freeze");
  }
  if (!printable_name(event_name)) {
    return make_error(errc::invalid_argument,
                      "event_registry::add: the event name is empty or not printable ASCII");
  }
  if (info.size != bytes || info.align != alignment) {
    return make_error(errc::invalid_argument,
                      "event_registry::add: the type_info size or alignment is not the type's");
  }
  if (by_name_.contains(event_name)) {
    return make_error(errc::already_exists, "event_registry::add: the event name repeats");
  }
  if (find_key(type_key)) {
    return make_error(errc::already_exists, "event_registry::add: the type is registered");
  }
  auto stored = types_.add(info);
  if (!stored) {
    return std::unexpected(stored.error());
  }
  entries_.push_back(entry{.name = std::string(event_name), .info = *stored, .type_key = type_key});
  const auto id = static_cast<std::uint32_t>(entries_.size());
  by_name_.emplace(std::string(event_name), id);
  return event_type_id{id};
}

result<event_type_id> event_registry::find(std::string_view event_name) const noexcept {
  if (const auto it = by_name_.find(event_name); it != by_name_.end()) {
    return event_type_id{it->second};
  }
  return make_error(errc::event_type_unknown, "event_registry::find: no such event type");
}

result<event_type_id> event_registry::find_key(const void *type_key) const noexcept {
  const auto it = std::ranges::find(entries_, type_key, &entry::type_key);
  if (it != entries_.end()) {
    return event_type_id{static_cast<std::uint32_t>(it - entries_.begin() + 1)};
  }
  return make_error(errc::event_type_unknown, "event_registry: the event type is not registered");
}

result<void> event_registry::freeze() noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "event_registry::freeze off the owner thread");
  }
  if (auto done = types_.freeze(); !done) {
    return done;
  }
  frozen_ = true;
  return {};
}

std::string_view event_registry::name(event_type_id id) const noexcept {
  if (id.value == 0 || id.value > entries_.size()) {
    return {};
  }
  return entries_[id.value - 1].name;
}

const type_info *event_registry::payload_type(event_type_id id) const noexcept {
  if (id.value == 0 || id.value > entries_.size()) {
    return nullptr;
  }
  return entries_[id.value - 1].info;
}

} // namespace sl::runtime
