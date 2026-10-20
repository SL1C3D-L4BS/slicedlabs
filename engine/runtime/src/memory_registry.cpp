// The memory tag registry (WO-079, SL-ARCH-003 ARCH3-R045, `immutability` tag registry, ADR-0023).
// Owner-thread until freeze; find and name are read-only and any-thread after it.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sl::runtime::mem {

namespace {

constexpr bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_tail(char c) noexcept {
  return is_lower(c) || (c >= '0' && c <= '9') || c == '_';
}

// ^[a-z][a-z0-9_]*(\.[a-z0-9_]+)*$
constexpr bool valid_name(std::string_view name) noexcept {
  if (name.empty() || !is_lower(name.front())) {
    return false;
  }
  bool segment_empty = false;
  for (const char c : name.substr(1)) {
    if (c == '.') {
      if (segment_empty) {
        return false;
      }
      segment_empty = true;
    } else if (is_tail(c)) {
      segment_empty = false;
    } else {
      return false;
    }
  }
  return !segment_empty;
}

static_assert(valid_name("runtime.root") && valid_name("a") && valid_name("ecs.chunks_2.x9"));
static_assert(!valid_name("") && !valid_name("A") && !valid_name("1a") && !valid_name("a.") &&
              !valid_name("a..b") && !valid_name(".a") && !valid_name("a-b"));

// The first segment is gpu or fabric.
constexpr bool reserved_name(std::string_view name) noexcept {
  const std::string_view first = name.substr(0, name.find('.'));
  return first == "gpu" || first == "fabric";
}

} // namespace

tag_registry::tag_registry() { names_.reserve(max_memory_tags); }

result<memory_tag> tag_registry::add(std::string_view name) { return insert(name, false); }

result<memory_tag> tag_registry::reserve_external(std::string_view name) {
  return insert(name, true);
}

result<memory_tag> tag_registry::insert(std::string_view name, bool external) {
  if (frozen()) {
    return make_error(errc::tag_registry_frozen,
                      "tag_registry: '" + std::string(name) + "' added after freeze");
  }
  if (!valid_name(name)) {
    return make_error(errc::invalid_argument,
                      "tag_registry: '" + std::string(name) +
                          "' does not match ^[a-z][a-z0-9_]*(\\.[a-z0-9_]+)*$");
  }
  const bool reserved = reserved_name(name);
  if (reserved && !external) {
    return make_error(errc::tag_reserved,
                      "tag_registry: '" + std::string(name) +
                          "' is reserved for reserve_external (gpu.*, fabric.*)");
  }
  if (!reserved && external) {
    return make_error(errc::invalid_argument,
                      "tag_registry: reserve_external takes gpu.* or fabric.* only, not '" +
                          std::string(name) + "'");
  }
  if (find(name).has_value()) {
    return make_error(errc::already_exists,
                      "tag_registry: '" + std::string(name) + "' is already registered");
  }
  if (names_.size() >= max_memory_tags) {
    return make_error(errc::out_of_range, "tag_registry: more than max_memory_tags (256) tags");
  }
  names_.emplace_back(name);
  const std::size_t id = names_.size();
  const std::span<bool> flags(external_);
  flags[id] = external;
  count_.store(id);
  return memory_tag{static_cast<std::uint16_t>(id)};
}

result<memory_tag> tag_registry::find(std::string_view name) const {
  const std::size_t n = size();
  for (std::size_t i = 0; i < n; ++i) {
    if (names_[i] == name) {
      return memory_tag{static_cast<std::uint16_t>(i + 1)};
    }
  }
  return make_error(errc::not_found, "tag_registry: no tag '" + std::string(name) + "'");
}

std::string_view tag_registry::name(memory_tag tag) const noexcept {
  if (tag.id == 0) {
    return "untagged";
  }
  if (!contains(tag)) {
    return "unknown";
  }
  return names_[tag.id - 1U];
}

result<void> tag_registry::freeze() noexcept {
  frozen_.store(true);
  return {};
}

bool tag_registry::frozen() const noexcept { return frozen_.load(); }

bool tag_registry::is_external(memory_tag tag) const noexcept {
  return contains(tag) && std::span(external_)[tag.id];
}

bool tag_registry::contains(memory_tag tag) const noexcept {
  return tag.id != 0 && tag.id <= size();
}

std::size_t tag_registry::size() const noexcept { return count_.load(); }

} // namespace sl::runtime::mem
