#pragma once

// The component registry (WO-088, SL-ARCH-003 ARCH3-R106 and R113, ADR-0025; episode 092). A
// component type is registered once, before the registry freezes, and gets its component id in
// registration order. A type must be nothrow move constructible and nothrow destructible, at most
// 2048 bytes and 64-byte aligned (component_type, a static check); add_raw checks the same limits
// at run time (component_invalid) for opaque payload components (seam V02-1). The layout digest is
// SHA-256 through det::state_hasher over (name, size, alignment, field layout) of every component
// in id order; it is an input of runtime_abi_digest (ARCH3-R032). Layer L5.
// Thread safety (`thread_safety`): add, add_raw and freeze are owner-thread before freeze; find,
// id_of, info, count and layout_digest are any-thread after freeze (read-only).

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/error.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime {
// The reflected description of a type (reflect.hpp, WO-087); the registry only stores the pointer.
struct type_info;
} // namespace sl::runtime

namespace sl::runtime::ecs {

// --- limits (SL-ARCH-003 `limits`) ---

/// The most component types of one registry (`limits` max_component_types).
inline constexpr std::uint32_t max_component_types = 1024;
/// The largest component (`limits` max_component_size).
inline constexpr std::uint32_t max_component_size = 2048;
/// The strictest component alignment (`limits` max_component_alignment).
inline constexpr std::uint32_t max_component_alignment = 64;

/// A component id: the registration index.
struct component_id {
  std::uint16_t value = 0;

  friend constexpr auto operator<=>(component_id, component_id) noexcept = default;
};

/// What a component type must be (ARCH3-R106): nothrow move constructible, nothrow destructible,
/// at most 2048 bytes and at most 64-byte aligned.
template <class T>
concept component_type =
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T> &&
    sizeof(T) <= max_component_size && alignof(T) <= max_component_alignment;

/// One registered component. move(dst, src) move-constructs a value at dst from the value at src
/// (src stays a valid moved-from value); destroy(p) ends the value at p. A trivially relocatable
/// value moves by memcpy and needs no destroy. type_key identifies the C++ type of add<T>
/// (nullptr for add_raw); reflect is the optional reflected description (reflect.hpp).
struct component_info {
  std::string_view name;
  std::uint32_t size = 0;
  std::uint32_t align = 0;
  bool trivially_relocatable = false;
  void (*move)(void *dst, void *src) noexcept = nullptr;
  void (*destroy)(void *p) noexcept = nullptr;
  const type_info *reflect = nullptr;
  const void *type_key = nullptr;
};

namespace detail {

/// One address per type: the type key of add<T> and id_of<T> (a constexpr object, never written).
template <class T> inline constexpr char type_anchor = 0;

template <class T> [[nodiscard]] constexpr const void *type_key_of() noexcept {
  return &type_anchor<std::remove_cvref_t<T>>;
}

template <class T> void move_value(void *dst, void *src) noexcept {
  ::new (dst) T(std::move(*static_cast<T *>(src)));
}

template <class T> void destroy_value(void *p) noexcept { std::destroy_at(static_cast<T *>(p)); }

/// The component_info of T under `name`.
template <class T>
[[nodiscard]] component_info info_of(std::string_view name, const type_info *reflect) noexcept {
  return component_info{.name = name,
                        .size = static_cast<std::uint32_t>(sizeof(T)),
                        .align = static_cast<std::uint32_t>(alignof(T)),
                        .trivially_relocatable = std::is_trivially_copyable_v<T>,
                        .move = &move_value<T>,
                        .destroy = &destroy_value<T>,
                        .reflect = reflect,
                        .type_key = type_key_of<T>()};
}

} // namespace detail

/// The registry of component types. Owner-thread until freeze, read-only (any-thread) after.
/// Neither copyable nor movable: worlds hold a pointer to it.
class component_registry {
public:
  component_registry();
  component_registry(const component_registry &) = delete;
  component_registry(component_registry &&) = delete;
  component_registry &operator=(const component_registry &) = delete;
  component_registry &operator=(component_registry &&) = delete;
  ~component_registry() = default;

  /// Registers T under `name`: as add_raw, and already_exists when T is already registered.
  template <component_type T>
  [[nodiscard]] result<component_id> add(std::string_view name,
                                         const type_info *reflect = nullptr) noexcept {
    return add_raw(detail::info_of<T>(name, reflect));
  }

  /// Registers a component: component_registry_frozen after freeze; component_invalid above
  /// 2048 bytes, a size of 0, an alignment that is not a power of two up to 64, no move or
  /// destroy function, or beyond 1024 types; invalid_argument for an empty name; already_exists
  /// for a registered name or type key. The name is copied.
  [[nodiscard]] result<component_id> add_raw(const component_info &desc) noexcept;
  /// The id of `name`; component_unknown when it is not registered.
  [[nodiscard]] result<component_id> find(std::string_view name) const noexcept;
  /// The id of T; component_unknown when T is not registered through add<T>.
  template <class T> [[nodiscard]] result<component_id> id_of() const noexcept {
    return find_key(detail::type_key_of<T>());
  }
  /// The id registered with `type_key`; component_unknown otherwise.
  [[nodiscard]] result<component_id> find_key(const void *type_key) const noexcept;
  /// The description of `id` (SL_ASSERT for an id that is not registered).
  [[nodiscard]] const component_info &info(component_id id) const noexcept;
  /// True for a registered id.
  [[nodiscard]] bool contains(component_id id) const noexcept { return id.value < count_; }
  /// Freezes the registry (runtime start); a second freeze succeeds and changes nothing.
  [[nodiscard]] result<void> freeze() noexcept;
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  /// SHA-256 over (name, size, alignment, field layout) of every component in id order.
  [[nodiscard]] fabric::digest layout_digest() const noexcept;
  /// The number of registered components.
  [[nodiscard]] std::uint16_t count() const noexcept { return count_; }

private:
  std::vector<std::string> names_;    // reserved to max_component_types: views stay valid
  std::vector<component_info> infos_; // reserved to max_component_types
  std::map<const void *, std::uint16_t, std::less<>> by_key_;
  std::uint16_t count_ = 0;
  bool frozen_ = false;
};

} // namespace sl::runtime::ecs
