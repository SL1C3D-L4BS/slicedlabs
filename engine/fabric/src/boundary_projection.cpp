// The read-only document projection (WO-061, SL-ARCH-001 ARCH1-R60, FAB-INV-BND-02).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

struct document_projection::data {
  explicit data(snapshot s) : snap(std::move(s)) {}

  snapshot snap;
  std::vector<std::string> domains;
  // The records of the projection, ascending by id.
  std::vector<snapshot_entry> entries;
  // Indices into `entries` of the records that belong to a document, ordered by (document, id).
  std::vector<std::size_t> by_document;
  std::vector<record_id> documents;
};

namespace {

bool valid_domain_name(const std::string &name) {
  if (is_domain_name(name)) {
    return true;
  }
  const reserved_prefix *row = find_reserved_prefix(name);
  return row != nullptr && row->prefix == name;
}

bool in_domains(std::span<const std::string> domains, const std::string &type_domain) {
  return !type_domain.empty() && std::ranges::any_of(domains, [&](const std::string &d) {
    return domain_within(type_domain, d);
  });
}

record_view view_of(const snapshot_entry &e) {
  return record_view{.id = e.id, .type = e.type, .body = &e.body, .provenance_inputs = {}};
}

} // namespace

result<document_projection> document_projection::make(const snapshot &from,
                                                      const schema_registry &schemas,
                                                      std::span<const std::string> domains) {
  if (domains.empty()) {
    return make_error(errc::invalid_argument, "a document projection needs at least one domain");
  }
  for (const std::string &d : domains) {
    if (!valid_domain_name(d)) {
      return make_error(
          errc::invalid_argument,
          std::format("'{}' is neither a dotted domain name nor a reserved prefix", d));
    }
  }
  auto d = std::make_shared<data>(from);
  d->domains.assign(domains.begin(), domains.end());
  std::ranges::sort(d->domains);
  const auto dup = std::ranges::unique(d->domains);
  d->domains.erase(dup.begin(), dup.end());

  const auto collect = [&](const snapshot_entry &e) -> result<void> {
    const type_desc *t = schemas.find(e.type);
    if (t == nullptr) {
      return make_error(errc::schema_unknown,
                        std::format("record {} has type {} which the schema registry does not know",
                                    e.id.to_string(), e.type.to_string()));
    }
    if (in_domains(d->domains, t->domain)) {
      d->entries.push_back(e);
    }
    return {};
  };
  if (auto r = from.for_each_entry(collect); !r) {
    return std::unexpected(std::move(r.error()));
  }
  // for_each_entry is ascending by id already; keep the invariant explicit for the binary searches.
  std::ranges::sort(d->entries, {}, &snapshot_entry::id);

  for (std::size_t i = 0; i < d->entries.size(); ++i) {
    if (!d->entries[i].document.is_null()) {
      d->by_document.push_back(i);
    }
  }
  std::ranges::sort(d->by_document, [&](std::size_t a, std::size_t b) {
    const snapshot_entry &x = d->entries[a];
    const snapshot_entry &y = d->entries[b];
    return x.document != y.document ? x.document < y.document : x.id < y.id;
  });
  for (const std::size_t i : d->by_document) {
    const record_id &doc = d->entries[i].document;
    if (d->documents.empty() || d->documents.back() != doc) {
      d->documents.push_back(doc);
    }
  }
  return document_projection(std::move(d));
}

result<record_view> document_projection::get(const record_id &id) const {
  const auto it = std::ranges::lower_bound(data_->entries, id, {}, &snapshot_entry::id);
  if (it != data_->entries.end() && it->id == id) {
    return view_of(*it);
  }
  auto live = data_->snap.contains(id);
  if (!live) {
    return std::unexpected(std::move(live.error()));
  }
  if (*live) {
    return make_error(errc::authority_denied,
                      std::format("record {} is outside the projection's domains", id.to_string()));
  }
  return make_error(errc::not_found, std::format("no live record {}", id.to_string()));
}

result<void> document_projection::check_scope(const record_id &id) const {
  auto v = get(id);
  if (!v) {
    return std::unexpected(std::move(v.error()));
  }
  return {};
}

result<void> document_projection::for_each(const visitor &fn) const {
  for (const snapshot_entry &e : data_->entries) {
    if (auto r = fn(view_of(e)); !r) {
      return r;
    }
  }
  return {};
}

std::span<const record_id> document_projection::documents() const noexcept {
  return data_->documents;
}

std::vector<record_id> document_projection::members(const record_id &document) const {
  std::vector<record_id> out;
  const auto range = std::ranges::equal_range(
      data_->by_document, document, {},
      [this](std::size_t i) -> const record_id & { return data_->entries[i].document; });
  out.reserve(static_cast<std::size_t>(range.size()));
  for (const std::size_t i : range) {
    out.push_back(data_->entries[i].id);
  }
  return out;
}

result<void> document_projection::for_each_in_document(const record_id &document,
                                                       const visitor &fn) const {
  const auto range = std::ranges::equal_range(
      data_->by_document, document, {},
      [this](std::size_t i) -> const record_id & { return data_->entries[i].document; });
  for (const std::size_t i : range) {
    if (auto r = fn(view_of(data_->entries[i])); !r) {
      return r;
    }
  }
  return {};
}

result<record_id> document_projection::document_of(const record_id &id) const {
  auto v = get(id);
  if (!v) {
    return std::unexpected(std::move(v.error()));
  }
  const auto it = std::ranges::lower_bound(data_->entries, id, {}, &snapshot_entry::id);
  return it->document;
}

std::size_t document_projection::size() const noexcept { return data_->entries.size(); }

std::span<const std::string> document_projection::domains() const noexcept {
  return data_->domains;
}

const digest &document_projection::root_digest() const noexcept {
  return data_->snap.root_digest();
}

const std::optional<digest> &document_projection::revision_digest() const noexcept {
  return data_->snap.revision_digest();
}

std::uint64_t document_projection::generation() const noexcept { return data_->snap.generation(); }

result<std::optional<record_state>> document_projection::read(const record_id &id) const {
  const auto it = std::ranges::lower_bound(data_->entries, id, {}, &snapshot_entry::id);
  if (it != data_->entries.end() && it->id == id) {
    return std::optional<record_state>(record_state{.type = it->type, .body = it->body});
  }
  auto live = data_->snap.contains(id);
  if (!live) {
    return std::unexpected(std::move(live.error()));
  }
  if (*live) {
    return make_error(errc::authority_denied,
                      std::format("record {} is outside the projection's domains", id.to_string()));
  }
  return std::optional<record_state>{};
}

} // namespace sl::fabric
