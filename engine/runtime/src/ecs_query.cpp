// ECS query terms and access sets (WO-090, SL-ARCH-003 ARCH3-R114, ADR-0025). init checks the
// resolved terms pairwise (two terms of one component conflict when they are the same term, with
// and without, without and any other term, or two argument terms such as read and write), then
// derives the required and excluded sets for matching and the access set: reads are read,
// optional<const T>, changed and added; writes are write and optional<T>; a component written is
// not listed among the reads. The match cache gets room for max_archetypes indices here, so
// iteration never grows it.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace sl::runtime::ecs::detail {

namespace {

// Requires the component in the archetype.
[[nodiscard]] bool requires_component(term_kind kind) noexcept {
  return kind == term_kind::read || kind == term_kind::write || kind == term_kind::with ||
         kind == term_kind::changed || kind == term_kind::added;
}

[[nodiscard]] bool reads_component(term_kind kind) noexcept {
  return kind == term_kind::read || kind == term_kind::optional_read ||
         kind == term_kind::changed || kind == term_kind::added;
}

[[nodiscard]] bool writes_component(term_kind kind) noexcept {
  return kind == term_kind::write || kind == term_kind::optional_write;
}

// Why two terms of one component cannot stand together, or nullptr when they can.
[[nodiscard]] const char *conflict(term_kind a, term_kind b) noexcept {
  if (a == b) {
    return "query: a term is given twice";
  }
  if (a == term_kind::without || b == term_kind::without) {
    return "query: without<T> with another term of the same T";
  }
  if (passes_argument(a) && passes_argument(b)) {
    return "query: two argument terms (read, write, optional) of the same T";
  }
  return nullptr;
}

// Sorts and deduplicates.
void normalise(std::vector<component_id> &ids) noexcept {
  std::ranges::sort(ids);
  const auto tail = std::ranges::unique(ids);
  ids.erase(tail.begin(), tail.end());
}

} // namespace

result<void> query_core::init(const component_registry &registry,
                              std::span<const query_term> terms) noexcept {
  if (terms.size() > max_query_terms) {
    return make_error(errc::query_invalid, "query: more than 32 terms");
  }
  for (std::size_t i = 0; i < terms.size(); ++i) {
    for (std::size_t j = i + 1; j < terms.size(); ++j) {
      if (terms[i].id != terms[j].id) {
        continue;
      }
      if (const char *why = conflict(terms[i].kind, terms[j].kind); why != nullptr) {
        return make_error(errc::query_invalid, why);
      }
    }
  }
  std::vector<component_id> required;
  std::vector<component_id> excluded;
  component_access derived;
  for (const query_term &t : terms) {
    if (requires_component(t.kind)) {
      required.push_back(t.id);
    }
    if (t.kind == term_kind::without) {
      excluded.push_back(t.id);
    }
    if (reads_component(t.kind)) {
      derived.reads.push_back(t.id);
    }
    if (writes_component(t.kind)) {
      derived.writes.push_back(t.id);
    }
  }
  normalise(required);
  normalise(excluded);
  normalise(derived.writes);
  std::erase_if(derived.reads, [&derived](component_id id) noexcept {
    return std::ranges::binary_search(derived.writes, id);
  });
  normalise(derived.reads);
  std::ranges::copy(required, required_.begin());
  std::ranges::copy(excluded, excluded_.begin());
  required_count_ = static_cast<std::uint32_t>(required.size());
  excluded_count_ = static_cast<std::uint32_t>(excluded.size());
  access_ = std::move(derived);
  registry_ = &registry;
  matched_.clear();
  matched_.reserve(max_archetypes);
  seen_ = nullptr;
  generation_ = 0;
  scanned_ = 0;
  return {};
}

} // namespace sl::runtime::ecs::detail
