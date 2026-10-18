// Immutable snapshots (WO-046, SL-ARCH-001 ARCH1-R51 to ARCH1-R53, ADR-0014).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric {

namespace {

std::unexpected<error> corrupt(std::string m) {
  return make_error(errc::object_corrupt, std::move(m));
}

std::vector<record_id> sorted_ids(const std::vector<tree_entry> &es) {
  std::vector<record_id> ids;
  ids.reserve(es.size());
  for (const tree_entry &e : es) {
    ids.push_back(e.id);
  }
  std::ranges::sort(ids);
  return ids;
}

} // namespace

result<snapshot> snapshot::empty(const store_backend &store) {
  auto root = encode_root_object(root_object{.records = empty_tree(), .membership = empty_tree()});
  if (!root) {
    return std::unexpected(root.error());
  }
  return snapshot(store, root->id, empty_tree(), empty_tree());
}

result<snapshot> snapshot::open_root(const store_backend &store, const digest &root) {
  auto o = store.get(root);
  if (!o) {
    return std::unexpected(o.error());
  }
  auto r = decode_root_object(*o);
  if (!r) {
    return std::unexpected(r.error());
  }
  return snapshot(store, root, r->records, r->membership);
}

result<snapshot> snapshot::open(const store_backend &store, const digest &revision) {
  auto o = store.get(revision);
  if (!o) {
    return std::unexpected(o.error());
  }
  auto rev = decode_revision(*o);
  if (!rev) {
    return std::unexpected(rev.error());
  }
  auto s = open_root(store, rev->root);
  if (!s) {
    return s;
  }
  s->revision_ = revision;
  s->generation_ = rev->generation;
  return s;
}

result<std::optional<tree_entry>> snapshot::entry(const record_id &id) const {
  return tree_reader(*store_).find(records_, id);
}

result<snapshot_entry> snapshot::load_record(const tree_entry &e) const {
  if (e.kind != entry_kind::record) {
    return corrupt("records trie entry of kind " + std::string(to_string(e.kind)));
  }
  auto o = store_->get(e.target);
  if (!o) {
    return std::unexpected(o.error());
  }
  auto r = decode_record_object(*o);
  if (!r) {
    return std::unexpected(r.error());
  }
  if (r->id != e.id) {
    return corrupt("record object of " + r->id.to_string() + " filed under " + e.id.to_string());
  }
  return snapshot_entry{.id = r->id,
                        .type = r->type,
                        .document = r->document,
                        .version = r->version,
                        .body = std::move(r->body),
                        .object = o->id};
}

result<snapshot_entry> snapshot::get(const record_id &id) const {
  auto e = entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  if (!*e) {
    return make_error(errc::not_found, "no record " + id.to_string());
  }
  if ((*e)->kind == entry_kind::tombstone) {
    return make_error(errc::not_found, "record " + id.to_string() + " is retired");
  }
  return load_record(**e);
}

result<bool> snapshot::contains(const record_id &id) const {
  auto e = entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  return e->has_value() && (*e)->kind == entry_kind::record;
}

result<std::uint64_t> snapshot::entry_count() const { return tree_reader(*store_).count(records_); }

result<std::vector<record_id>> snapshot::members(const record_id &doc) const {
  const tree_reader reader(*store_);
  auto d = reader.find(membership_, doc);
  if (!d) {
    return std::unexpected(d.error());
  }
  if (!*d) {
    return std::vector<record_id>{};
  }
  if ((*d)->kind != entry_kind::document) {
    return corrupt("membership map entry of kind " + std::string(to_string((*d)->kind)));
  }
  auto es = reader.entries((*d)->target);
  if (!es) {
    return std::unexpected(es.error());
  }
  if (es->empty()) {
    return corrupt("membership map names an empty document " + doc.to_string());
  }
  for (const tree_entry &e : *es) {
    if (e.kind != entry_kind::member) {
      return corrupt("member trie entry of kind " + std::string(to_string(e.kind)));
    }
  }
  return sorted_ids(*es);
}

result<std::vector<record_id>> snapshot::documents() const {
  auto es = tree_reader(*store_).entries(membership_);
  if (!es) {
    return std::unexpected(es.error());
  }
  for (const tree_entry &e : *es) {
    if (e.kind != entry_kind::document) {
      return corrupt("membership map entry of kind " + std::string(to_string(e.kind)));
    }
  }
  return sorted_ids(*es);
}

result<void> snapshot::for_each_in_document(const record_id &doc, const entry_visitor &fn) const {
  auto ids = members(doc);
  if (!ids) {
    return std::unexpected(ids.error());
  }
  for (const record_id &id : *ids) {
    auto e = get(id);
    if (!e) {
      if (e.error().code == errc::not_found) {
        return corrupt("member " + id.to_string() + " of " + doc.to_string() + " is not live");
      }
      return std::unexpected(e.error());
    }
    if (e->document != doc) {
      return corrupt("member " + id.to_string() + " belongs to " + e->document.to_string());
    }
    auto r = fn(*e);
    if (!r) {
      return r;
    }
  }
  return {};
}

result<void> snapshot::for_each_entry(const entry_visitor &fn) const {
  auto es = tree_reader(*store_).entries(records_);
  if (!es) {
    return std::unexpected(es.error());
  }
  std::erase_if(*es, [](const tree_entry &e) { return e.kind == entry_kind::tombstone; });
  std::ranges::sort(*es, {}, &tree_entry::id);
  for (const tree_entry &e : *es) {
    auto rec = load_record(e);
    if (!rec) {
      return std::unexpected(rec.error());
    }
    auto r = fn(*rec);
    if (!r) {
      return r;
    }
  }
  return {};
}

result<std::vector<record_delta>> snapshot::diff(const snapshot &other,
                                                 tree_diff_stats *stats) const {
  std::vector<record_delta> out;
  auto r = tree_reader(*store_).diff(records_, other.records_, out, stats);
  if (!r) {
    return std::unexpected(r.error());
  }
  std::ranges::sort(out, {}, &record_delta::id);
  return out;
}

result<std::optional<record_state>> snapshot::read(const record_id &id) const {
  auto e = entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  if (!*e || (*e)->kind == entry_kind::tombstone) {
    return std::optional<record_state>{};
  }
  auto rec = load_record(**e);
  if (!rec) {
    return std::unexpected(rec.error());
  }
  return std::optional<record_state>{record_state{.type = rec->type, .body = std::move(rec->body)}};
}

result<void> snapshot::for_each(const visitor &fn) const {
  return for_each_entry([&fn](const snapshot_entry &e) -> result<void> {
    return fn(record_view{.id = e.id, .type = e.type, .body = &e.body, .provenance_inputs = {}});
  });
}

} // namespace sl::fabric
