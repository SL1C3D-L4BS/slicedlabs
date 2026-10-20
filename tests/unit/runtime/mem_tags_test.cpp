// RT-INV-MEM-05 (SL-ARCH-003 ARCH3-R045, `immutability` tag registry, ADR-0023): add after freeze
// is tag_registry_frozen, gpu.* or fabric.* through add is tag_reserved and through
// reserve_external succeeds, a duplicate name is already_exists. Also: the name grammar, the
// max_memory_tags limit, find and name, and the runtime tags WO-096 registers at start.

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

namespace mem = sl::runtime::mem;
using sl::errc;

template <typename T> errc code_of(const sl::result<T> &r) {
  return r.has_value() ? errc::ok : r.error().code;
}

} // namespace

TEST_CASE("RT-INV-MEM-05: add after freeze is tag_registry_frozen") {
  mem::tag_registry registry;
  const auto root = registry.add("runtime.root");
  REQUIRE(root.has_value());
  CHECK_FALSE(registry.frozen());
  REQUIRE(registry.freeze().has_value());
  CHECK(registry.frozen());
  CHECK(registry.freeze().has_value()); // a second freeze changes nothing
  CHECK(code_of(registry.add("jobs.records")) == errc::tag_registry_frozen);
  CHECK(code_of(registry.reserve_external("gpu.heaps")) == errc::tag_registry_frozen);
  // still readable after freeze
  CHECK(registry.find("runtime.root").value() == *root);
  CHECK(registry.name(*root) == "runtime.root");
  CHECK(registry.size() == 1);
}

TEST_CASE("RT-INV-MEM-05: gpu.* and fabric.* are tag_reserved through add and succeed through "
          "reserve_external") {
  mem::tag_registry registry;
  CHECK(code_of(registry.add("gpu.heaps")) == errc::tag_reserved);
  CHECK(code_of(registry.add("fabric.projection")) == errc::tag_reserved);
  CHECK(code_of(registry.add("gpu")) == errc::tag_reserved);
  const auto heaps = registry.reserve_external("gpu.heaps");
  const auto projection = registry.reserve_external("fabric.projection");
  REQUIRE(heaps.has_value());
  REQUIRE(projection.has_value());
  CHECK(registry.is_external(*heaps));
  CHECK(registry.is_external(*projection));
  CHECK(code_of(registry.reserve_external("events")) == errc::invalid_argument);
  CHECK(code_of(registry.reserve_external("gpux.heaps")) == errc::invalid_argument);
  const auto gpux = registry.add("gpux.heaps"); // only the whole first segment is reserved
  REQUIRE(gpux.has_value());
  CHECK_FALSE(registry.is_external(*gpux));
}

TEST_CASE("RT-INV-MEM-05: a duplicate name is already_exists") {
  mem::tag_registry registry;
  REQUIRE(registry.add("events").has_value());
  CHECK(code_of(registry.add("events")) == errc::already_exists);
  REQUIRE(registry.reserve_external("fabric.projection").has_value());
  CHECK(code_of(registry.reserve_external("fabric.projection")) == errc::already_exists);
  CHECK(registry.size() == 2);
}

TEST_CASE("RT-INV-MEM-05: names follow the grammar and the registry holds max_memory_tags") {
  mem::tag_registry registry;
  constexpr std::array<std::string_view, 9> bad = {"",     "Runtime", "1abc", "a.", ".a",
                                                   "a..b", "a-b",     "a b",  "a.B"};
  for (const std::string_view name : bad) {
    CHECK(code_of(registry.add(name)) == errc::invalid_argument);
  }
  CHECK(registry.size() == 0);
  for (std::size_t i = 0; i < mem::max_memory_tags; ++i) {
    const auto t = registry.add("t" + std::to_string(i));
    REQUIRE(t.has_value());
    CHECK(t->id == i + 1);
  }
  CHECK(code_of(registry.add("one.more")) == errc::out_of_range);
  CHECK(registry.size() == mem::max_memory_tags);
  CHECK(registry.name(mem::memory_tag{256}) == "t255");
}

TEST_CASE("RT-INV-MEM-05: the runtime tags register, find and name agree, unknown tags are named") {
  mem::tag_registry registry;
  for (const std::string_view name : mem::runtime_tag_names) {
    REQUIRE(registry.add(name).has_value());
  }
  for (const std::string_view name : mem::runtime_external_tag_names) {
    REQUIRE(registry.reserve_external(name).has_value());
  }
  REQUIRE(registry.freeze().has_value());
  CHECK(registry.size() == 10);
  for (const std::string_view name : mem::runtime_tag_names) {
    const auto t = registry.find(name);
    REQUIRE(t.has_value());
    CHECK(registry.name(*t) == name);
    CHECK_FALSE(registry.is_external(*t));
  }
  const auto projection = registry.find("fabric.projection");
  REQUIRE(projection.has_value());
  CHECK(registry.is_external(*projection));
  CHECK(code_of(registry.find("missing")) == errc::not_found);
  CHECK(registry.name(mem::memory_tag{}) == "untagged");
  CHECK(registry.name(mem::memory_tag{200}) == "unknown");
  CHECK_FALSE(registry.contains(mem::memory_tag{}));
}
