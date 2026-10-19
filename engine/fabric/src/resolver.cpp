// The reference resolver (WO-060, SL-ARCH-001 ARCH1-R63 to ARCH1-R66; ADR-0009, ADR-0011).

#include "sl/fabric/resolver.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

namespace {

constexpr std::string_view import_record_type_name = "fabric.identity.import_record";

std::unexpected<error> corrupt(std::string m) {
  return make_error(errc::object_corrupt, std::move(m));
}

// The manifest's pins in oset order (order key, then element id), the first pin of each package.
std::vector<package_pin> ordered_pins(const project_manifest &m) {
  std::vector<const gen::oset_item<package_pin> *> items;
  items.reserve(m.package_pins.size());
  for (const auto &item : m.package_pins) {
    items.push_back(&item);
  }
  std::ranges::sort(items, [](const auto *a, const auto *b) {
    if (a->order_key != b->order_key) {
      return a->order_key < b->order_key;
    }
    return a->element < b->element;
  });
  std::vector<package_pin> out;
  out.reserve(items.size());
  for (const auto *item : items) {
    const bool seen = std::ranges::any_of(
        out, [&](const package_pin &p) { return p.package == item->v.package; });
    if (!seen) {
      out.push_back(item->v);
    }
  }
  return out;
}

bool accepted(const std::vector<type_id> &expected, const type_id &type) {
  return expected.empty() || std::ranges::find(expected, type) != expected.end();
}

// The outcome of the leaf `leaf` of `target` in snapshot `s` (found in `scope`).
result<resolution> decide(const snapshot &s, const tree_entry &leaf, const record_id &target,
                          const std::vector<type_id> &expected, resolution_scope scope,
                          const std::optional<record_id> &package) {
  resolution r;
  r.scope = scope;
  r.package = package;
  r.target = target;
  if (leaf.kind == entry_kind::tombstone) {
    r.outcome = resolution_outcome::retired;
    return r;
  }
  if (leaf.kind != entry_kind::record) {
    return corrupt("records trie entry of kind " + std::string(to_string(leaf.kind)) + " for " +
                   target.to_string());
  }
  auto e = s.get(target);
  if (!e) {
    return std::unexpected(e.error());
  }
  r.outcome =
      accepted(expected, e->type) ? resolution_outcome::resolved : resolution_outcome::wrong_type;
  r.record = std::move(*e);
  return r;
}

// Whether `target` is a member of document `doc` in the membership map of `s`.
result<bool> is_member(const snapshot &s, const record_id &doc, const record_id &target) {
  const tree_reader reader(s.store());
  auto d = reader.find(s.membership_root(), doc);
  if (!d) {
    return std::unexpected(d.error());
  }
  if (!*d) {
    return false;
  }
  if ((*d)->kind != entry_kind::document) {
    return corrupt("membership map entry of kind " + std::string(to_string((*d)->kind)));
  }
  auto m = reader.find((*d)->target, target);
  if (!m) {
    return std::unexpected(m.error());
  }
  if (!*m) {
    return false;
  }
  if ((*m)->kind != entry_kind::member) {
    return corrupt("member trie entry of kind " + std::string(to_string((*m)->kind)));
  }
  return true;
}

resolution package_missing(const record_id &package, const record_id &target) {
  resolution r;
  r.outcome = resolution_outcome::package_missing;
  r.package = package;
  r.target = target;
  return r;
}

resolution missing(const std::optional<record_id> &package, const record_id &target) {
  resolution r;
  r.outcome = resolution_outcome::missing;
  r.package = package;
  r.target = target;
  return r;
}

} // namespace

std::string_view to_string(resolution_outcome o) noexcept {
  switch (o) {
  case resolution_outcome::resolved:
    return "resolved";
  case resolution_outcome::retired:
    return "retired";
  case resolution_outcome::missing:
    return "missing";
  case resolution_outcome::wrong_type:
    return "wrong_type";
  case resolution_outcome::package_missing:
    return "package_missing";
  }
  return "unknown";
}

std::string_view to_string(resolution_scope s) noexcept {
  switch (s) {
  case resolution_scope::none:
    return "none";
  case resolution_scope::document:
    return "document";
  case resolution_scope::project:
    return "project";
  case resolution_scope::package:
    return "package";
  }
  return "unknown";
}

record_id import_record_id_for(std::string_view system, std::string_view locator) noexcept {
  return imported(id_class::import, system, locator);
}

resolver::resolver(snapshot project, const project_manifest &manifest, package_provider &packages)
    : project_(std::move(project)), pins_(ordered_pins(manifest)), packages_(&packages) {}

const package_pin *resolver::pin_of(const record_id &package) const noexcept {
  const auto it =
      std::ranges::find_if(pins_, [&](const package_pin &p) { return p.package == package; });
  return it == pins_.end() ? nullptr : &*it;
}

result<std::optional<snapshot>> resolver::open_pin(const package_pin &pin) const {
  auto s = packages_->open(pin.package, pin.revision);
  if (!s) {
    if (s.error().code == errc::not_found || s.error().code == errc::object_missing) {
      return std::optional<snapshot>{};
    }
    return std::unexpected(s.error());
  }
  if (s->revision_digest() != std::optional<digest>(pin.revision)) {
    return make_error(errc::invalid_argument,
                      "package provider gave package " + pin.package.to_string() +
                          " at another revision than its pin " + pin.revision.to_hex());
  }
  return std::optional<snapshot>(std::move(*s));
}

result<resolution> resolver::resolve(const reference &ref, const record_id &from_document) const {
  if (ref.external) {
    if (!ref.target.is_null()) {
      return make_error(errc::invalid_argument,
                        "a reference names both a target and an external reference");
    }
    return resolve_external(ref, *ref.external, from_document);
  }
  if (ref.target.is_null()) {
    return make_error(errc::invalid_argument, "a reference names no target");
  }
  return resolve_id(ref.package, ref.target, ref.expected, from_document);
}

result<resolution> resolver::resolve_id(const std::optional<record_id> &package,
                                        const record_id &target,
                                        const std::vector<type_id> &expected,
                                        const record_id &from_document) const {
  if (package) {
    // A cross-package reference: only the pinned revision of that package (ARCH1-R66).
    const package_pin *pin = pin_of(*package);
    if (pin == nullptr) {
      return package_missing(*package, target);
    }
    auto s = open_pin(*pin);
    if (!s) {
      return std::unexpected(s.error());
    }
    if (!*s) {
      return package_missing(*package, target);
    }
    auto leaf = (*s)->entry(target);
    if (!leaf) {
      return std::unexpected(leaf.error());
    }
    if (!*leaf) {
      return missing(package, target);
    }
    return decide(**s, **leaf, target, expected, resolution_scope::package, package);
  }

  // 1. The same document, through the membership map.
  if (!from_document.is_null()) {
    auto member = is_member(project_, from_document, target);
    if (!member) {
      return std::unexpected(member.error());
    }
    if (*member) {
      auto leaf = project_.entry(target);
      if (!leaf) {
        return std::unexpected(leaf.error());
      }
      if (!*leaf || (*leaf)->kind != entry_kind::record) {
        return corrupt("member " + target.to_string() + " of " + from_document.to_string() +
                       " is not live");
      }
      return decide(project_, **leaf, target, expected, resolution_scope::document, std::nullopt);
    }
  }
  // 2. The project.
  auto leaf = project_.entry(target);
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  if (*leaf) {
    return decide(project_, **leaf, target, expected, resolution_scope::project, std::nullopt);
  }
  // 3. The pinned packages, in pin order.
  for (const package_pin &pin : pins_) {
    auto s = open_pin(pin);
    if (!s) {
      return std::unexpected(s.error());
    }
    if (!*s) {
      return package_missing(pin.package, target);
    }
    auto found = (*s)->entry(target);
    if (!found) {
      return std::unexpected(found.error());
    }
    if (*found) {
      return decide(**s, **found, target, expected, resolution_scope::package, pin.package);
    }
  }
  // 4. Unresolved.
  return missing(std::nullopt, target);
}

result<resolution> resolver::resolve_external(const reference &ref, const external_ref &ext,
                                              const record_id &from_document) const {
  if (ext.system.empty() || ext.locator.empty()) {
    return make_error(errc::invalid_argument, "an external reference needs a system and a locator");
  }
  // The scope holding the import record: the pinned package, or the project.
  std::optional<snapshot> scope_snapshot;
  if (ref.package) {
    const package_pin *pin = pin_of(*ref.package);
    if (pin == nullptr) {
      return package_missing(*ref.package, record_id{});
    }
    auto s = open_pin(*pin);
    if (!s) {
      return std::unexpected(s.error());
    }
    if (!*s) {
      return package_missing(*ref.package, record_id{});
    }
    scope_snapshot = std::move(*s);
  }
  const snapshot &scope = scope_snapshot ? *scope_snapshot : project_;
  const resolution_scope where =
      ref.package ? resolution_scope::package : resolution_scope::project;

  const record_id import_id = import_record_id_for(ext.system, ext.locator);
  auto leaf = scope.entry(import_id);
  if (!leaf) {
    return std::unexpected(leaf.error());
  }
  if (!*leaf) {
    return missing(ref.package, record_id{});
  }
  if ((*leaf)->kind == entry_kind::tombstone) {
    resolution r;
    r.outcome = resolution_outcome::retired;
    r.scope = where;
    r.package = ref.package;
    return r;
  }
  auto e = scope.get(import_id);
  if (!e) {
    return std::unexpected(e.error());
  }
  if (e->type != type_id_of(import_record_type_name)) {
    return corrupt("the import record id " + import_id.to_string() +
                   " holds a record of another type");
  }
  auto rec = import_record::from_value(e->body);
  if (!rec) {
    return corrupt("the import record " + import_id.to_string() +
                   " does not decode: " + rec.error().message);
  }
  if (auto ok = check_import_record(*rec); !ok) {
    return corrupt("the import record " + import_id.to_string() +
                   " is ill-formed: " + ok.error().message);
  }
  if (rec->system != ext.system || rec->locator != ext.locator) {
    return corrupt("the import record " + import_id.to_string() +
                   " maps another system or locator");
  }
  if (rec->record.scheme() != mint_scheme::imported) {
    return corrupt("the import record " + import_id.to_string() +
                   " names a record id outside the imported scheme");
  }
  auto r = resolve_id(ref.package, rec->record, ref.expected, from_document);
  if (!r) {
    return r;
  }
  r->import = std::move(*rec);
  return r;
}

} // namespace sl::fabric
