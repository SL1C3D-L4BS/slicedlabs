// document_source, binding_registry, the sinks, the outbox and body_bytes (WO-097, SL-ARCH-003
// ARCH3-R148, R151, R152, R154, R158; ADR-0027).

#include "sl/runtime/bridge.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace sl::runtime::link {

namespace fab = sl::fabric;

proposal_sink::~proposal_sink() = default;

result<void> null_sink::submit(const fab::patch &p, const fab::tx_origin &origin) noexcept {
  ++submissions_;
  changes_ += p.ops.size();
  last_ = p;
  last_origin_ = origin;
  return {};
}

// --- document_source ---

document_source::document_source(fab::document_projection projection,
                                 std::vector<load_entry> plan) noexcept
    : projection_(std::move(projection)), plan_(std::move(plan)) {}

result<document_source> document_source::make(fab::document_projection projection) noexcept {
  std::vector<load_entry> plan;
  plan.reserve(projection.size());
  // Records of no document come first: their group key is the null id, below every document.
  const auto loose = projection.for_each([&](const fab::record_view &v) -> result<void> {
    auto doc = projection.document_of(v.id);
    if (!doc) {
      return std::unexpected(std::move(doc.error()));
    }
    if (doc->is_null()) {
      plan.push_back(load_entry{.document = fab::record_id{}, .record = v.id, .type = v.type});
    }
    return {};
  });
  if (!loose) {
    return std::unexpected(loose.error());
  }
  for (const fab::record_id &doc : projection.documents()) {
    const auto in_doc =
        projection.for_each_in_document(doc, [&](const fab::record_view &v) -> result<void> {
          plan.push_back(load_entry{.document = doc, .record = v.id, .type = v.type});
          return {};
        });
    if (!in_doc) {
      return std::unexpected(in_doc.error());
    }
  }
  return document_source(std::move(projection), std::move(plan));
}

// --- binding_registry ---

result<void> binding_registry::add(ecs::component_id component, fabric_binding binding) noexcept {
  if (binding.type() == nullptr) {
    return make_error(errc::invalid_argument, "binding_registry::add: a binding with no type");
  }
  const bool taken = std::ranges::any_of(entries_, [&](const entry &e) noexcept {
    return e.component == component || e.binding.fabric_type() == binding.fabric_type();
  });
  if (taken) {
    return make_error(errc::already_exists,
                      "binding_registry::add: the component or the Fabric type is already bound");
  }
  const auto at = std::ranges::lower_bound(entries_, component, {}, &entry::component);
  entries_.insert(at, entry{.component = component, .binding = std::move(binding)});
  return {};
}

const binding_registry::entry *binding_registry::entry_of(const fab::type_id &type) const noexcept {
  const auto it = std::ranges::find_if(
      entries_, [&](const entry &e) noexcept { return e.binding.fabric_type() == type; });
  return it == entries_.end() ? nullptr : &*it;
}

const fabric_binding *binding_registry::find(const fab::type_id &type) const noexcept {
  const entry *e = entry_of(type);
  return e == nullptr ? nullptr : &e->binding;
}

const binding_registry::entry *
binding_registry::find_component(ecs::component_id component) const noexcept {
  const auto it = std::ranges::lower_bound(entries_, component, {}, &entry::component);
  return it == entries_.end() || it->component != component ? nullptr : &*it;
}

// --- outbox ---

outbox::buffer &outbox::add_buffer() {
  buffers_.push_back(std::make_unique<buffer>());
  return *buffers_.back();
}

std::size_t outbox::pending() const noexcept {
  return std::accumulate(
      buffers_.begin(), buffers_.end(), std::size_t{0},
      [](std::size_t n, const std::unique_ptr<buffer> &b) noexcept { return n + b->size(); });
}

void outbox::clear() noexcept {
  for (const std::unique_ptr<buffer> &b : buffers_) {
    b->clear();
  }
}

namespace {

// The outbox order: record id bytes, then the path (field number ascending, path_segment order).
bool change_less(const fab::observed_change &a, const fab::observed_change &b) noexcept {
  if (a.record != b.record) {
    return a.record < b.record;
  }
  return a.path < b.path;
}

bool same_target(const fab::observed_change &a, const fab::observed_change &b) noexcept {
  return a.record == b.record && a.path == b.path;
}

struct collect_change {
  std::vector<fab::observed_change> *into = nullptr;
  void operator()(const det::ordered_key & /*key*/, fab::observed_change &c) const {
    into->push_back(std::move(c));
  }
};

} // namespace

std::vector<fab::observed_change> outbox::drain() {
  std::vector<buffer *> heads;
  heads.reserve(buffers_.size());
  for (const std::unique_ptr<buffer> &b : buffers_) {
    // cppcheck-suppress useStlAlgorithm ; collecting raw pointers reads plainer as a loop
    heads.push_back(b.get());
  }
  std::vector<fab::observed_change> merged;
  merged.reserve(pending());
  det::merge_ordered(std::span<buffer *const>(heads), collect_change{.into = &merged});
  clear();
  // Stable: equal targets keep their ordered_key order, so the last of each run is the latest.
  std::ranges::stable_sort(merged, change_less);
  std::vector<fab::observed_change> out;
  out.reserve(merged.size());
  for (fab::observed_change &c : merged) {
    if (!out.empty() && same_target(out.back(), c)) {
      out.back() = std::move(c);
    } else {
      out.push_back(std::move(c));
    }
  }
  return out;
}

// --- body bytes ---

std::uint64_t body_bytes(const fab::value &v) noexcept {
  switch (v.kind()) {
  case fab::value_kind::bool_:
    return 1;
  case fab::value_kind::uint:
  case fab::value_kind::nint:
  case fab::value_kind::f64:
    return 8;
  case fab::value_kind::f32:
    return 4;
  case fab::value_kind::string:
    return v.as_string().size();
  case fab::value_kind::bytes:
    return v.as_bytes().size();
  case fab::value_kind::id:
    return 16;
  case fab::value_kind::digest:
    return 33;
  case fab::value_kind::list: {
    std::uint64_t n = 0;
    for (const fab::value &item : v.items()) {
      // cppcheck-suppress useStlAlgorithm ; a recursive sum reads plainer as a loop
      n += body_bytes(item);
    }
    return n;
  }
  case fab::value_kind::map: {
    std::uint64_t n = 0;
    for (const fab::map_entry &e : v.entries()) {
      // cppcheck-suppress useStlAlgorithm ; a recursive sum reads plainer as a loop
      n += body_bytes(e.key) + body_bytes(e.v);
    }
    return n;
  }
  case fab::value_kind::struct_: {
    std::uint64_t n = 0;
    for (const fab::field &f : v.fields()) {
      // cppcheck-suppress useStlAlgorithm ; a recursive sum reads plainer as a loop
      n += 4 + body_bytes(f.v);
    }
    return n;
  }
  case fab::value_kind::variant:
    return 4 + body_bytes(v.payload());
  case fab::value_kind::oset: {
    std::uint64_t n = 0;
    for (const fab::oset_entry &e : v.elements()) {
      // cppcheck-suppress useStlAlgorithm ; a recursive sum reads plainer as a loop
      n += 16 + e.order_key.size() + body_bytes(e.v);
    }
    return n;
  }
  }
  return 0;
}

std::uint64_t projection_bytes(const fab::document_projection &projection) {
  std::uint64_t n = 0;
  const auto walked = projection.for_each([&n](const fab::record_view &v) -> result<void> {
    if (v.body != nullptr) {
      n += body_bytes(*v.body);
    }
    return {};
  });
  static_cast<void>(walked);
  return n;
}

} // namespace sl::runtime::link
