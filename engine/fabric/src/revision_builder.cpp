// The revision builder (WO-046, SL-ARCH-001 ARCH1-R22, ARCH1-R50 to ARCH1-R52, SL-ARCH-002
// ARCH2-R07 step C3, ADR-0014).

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric {

namespace {

// The base state of one record: live or not and, when live, its document.
struct base_state {
  std::optional<tree_entry> entry;
  bool live = false;
  record_id document;
};

result<base_state> base_of(const snapshot &base, const record_id &id) {
  base_state out;
  auto e = base.entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  out.entry = *e;
  if (!out.entry || out.entry->kind != entry_kind::record) {
    return out;
  }
  auto rec = base.get(id);
  if (!rec) {
    return std::unexpected(rec.error());
  }
  out.live = true;
  out.document = rec->document;
  return out;
}

tree_change member_change(const record_id &id, bool add) {
  tree_change c{.id = id, .entry = std::nullopt};
  if (add) {
    c.entry = tree_entry{.id = id, .kind = entry_kind::member, .target = {}};
  }
  return c;
}

} // namespace

result<void> revision_builder::put(const record_id &id, record_write w) {
  if (id.is_null() || w.type.is_null()) {
    return make_error(errc::invalid_argument, "a record write needs an id and a type");
  }
  changes_.insert_or_assign(id, std::optional<record_write>(std::move(w)));
  return {};
}

result<void> revision_builder::retire(const record_id &id) {
  if (id.is_null()) {
    return make_error(errc::invalid_argument, "cannot retire the null id");
  }
  changes_.insert_or_assign(id, std::nullopt);
  return {};
}

result<void> revision_builder::stage(std::span<const staged_change> changes,
                                     const document_resolver &document_of) {
  for (const staged_change &c : changes) {
    if (!c.after) {
      auto live = base_.contains(c.id);
      if (!live) {
        return std::unexpected(live.error());
      }
      if (*live) {
        auto r = retire(c.id);
        if (!r) {
          return r;
        }
      } else {
        changes_.erase(c.id); // created and removed again: nothing to write
      }
      continue;
    }
    record_id doc;
    if (document_of) {
      auto d = document_of(c.id, *c.after);
      if (!d) {
        return std::unexpected(d.error());
      }
      doc = *d;
    } else {
      auto b = base_of(base_, c.id);
      if (!b) {
        return std::unexpected(b.error());
      }
      doc = b->document;
    }
    auto r = put(c.id, record_write{.type = c.after->type,
                                    .document = doc,
                                    .body = c.after->body,
                                    .version = std::nullopt});
    if (!r) {
      return r;
    }
  }
  return {};
}

result<built_revision> revision_builder::build(const revision_inputs &in) const {
  const store_backend &store = base_.store();
  const tree_reader reader(store);
  object_sink sink(store);

  std::vector<tree_change> records;
  std::map<record_id, std::vector<tree_change>> by_document;
  records.reserve(changes_.size());
  for (const auto &[id, change] : changes_) {
    auto old = base_of(base_, id);
    if (!old) {
      return std::unexpected(old.error());
    }
    record_id new_doc;
    if (change) {
      type_version version;
      if (change->version) {
        version = *change->version;
      } else if (const type_desc *t = schemas_->find(change->type)) {
        version = t->version;
      } else {
        return make_error(errc::schema_unknown, "record " + id.to_string() +
                                                    " has unregistered type " +
                                                    change->type.id().to_string());
      }
      auto o = encode_record_object(record_object{.id = id,
                                                  .type = change->type,
                                                  .document = change->document,
                                                  .version = version,
                                                  .body = change->body});
      if (!o) {
        return std::unexpected(o.error());
      }
      records.push_back(tree_change{
          .id = id, .entry = tree_entry{.id = id, .kind = entry_kind::record, .target = o->id}});
      auto added = sink.add(std::move(*o));
      if (!added) {
        return std::unexpected(added.error());
      }
      new_doc = change->document;
    } else {
      if (!old->live || !old->entry) {
        return make_error(errc::not_found, "cannot retire " + id.to_string() + ": not live");
      }
      if (in.tx_id.is_null()) {
        return make_error(errc::invalid_argument, "retiring an id needs the tx id");
      }
      auto o = encode_tombstone(gen::fabric_identity::tombstone{
          .id = id, .last_record = old->entry->target, .retired_by = in.tx_id});
      if (!o) {
        return std::unexpected(o.error());
      }
      records.push_back(tree_change{
          .id = id, .entry = tree_entry{.id = id, .kind = entry_kind::tombstone, .target = o->id}});
      auto added = sink.add(std::move(*o));
      if (!added) {
        return std::unexpected(added.error());
      }
    }
    const bool new_live = change.has_value();
    if (old->live && !old->document.is_null() && (!new_live || new_doc != old->document)) {
      by_document[old->document].push_back(member_change(id, false));
    }
    if (new_live && !new_doc.is_null() && (!old->live || old->document != new_doc)) {
      by_document[new_doc].push_back(member_change(id, true));
    }
  }

  auto records_root = update_tree(reader, base_.records_root(), records, sink);
  if (!records_root) {
    return std::unexpected(records_root.error());
  }

  std::vector<tree_change> documents;
  documents.reserve(by_document.size());
  for (auto &[doc, member_changes] : by_document) {
    auto cur = reader.find(base_.membership_root(), doc);
    if (!cur) {
      return std::unexpected(cur.error());
    }
    digest members_root = empty_tree();
    if (*cur) {
      if ((*cur)->kind != entry_kind::document) {
        return make_error(errc::object_corrupt, "membership map entry is not a document");
      }
      members_root = (*cur)->target;
    }
    auto next = update_tree(reader, members_root, member_changes, sink);
    if (!next) {
      return std::unexpected(next.error());
    }
    tree_change c{.id = doc, .entry = std::nullopt};
    if (next->count > 0) {
      c.entry = tree_entry{.id = doc, .kind = entry_kind::document, .target = next->id};
    }
    documents.push_back(c);
  }
  auto membership_root = update_tree(reader, base_.membership_root(), documents, sink);
  if (!membership_root) {
    return std::unexpected(membership_root.error());
  }

  built_revision out;
  out.root_value = root_object{.records = records_root->id, .membership = membership_root->id};
  auto root = encode_root_object(out.root_value);
  if (!root) {
    return std::unexpected(root.error());
  }
  out.root = root->id;
  auto added_root = sink.add(std::move(*root));
  if (!added_root) {
    return std::unexpected(added_root.error());
  }

  auto tx = encode_tx_record(in.tx);
  if (!tx) {
    return std::unexpected(tx.error());
  }
  out.tx = tx->id;
  auto added_tx = sink.add(std::move(*tx));
  if (!added_tx) {
    return std::unexpected(added_tx.error());
  }

  gen::fabric_history::revision rev;
  std::optional<std::uint64_t> top_generation;
  if (base_.revision_digest()) {
    rev.parents.push_back(*base_.revision_digest());
    top_generation = base_.generation();
  }
  for (const digest &p : in.extra_parents) {
    if (std::ranges::find(rev.parents, p) != rev.parents.end()) {
      return make_error(errc::invalid_argument, "parent revision given twice: " + p.to_hex());
    }
    auto o = store.get(p);
    if (!o) {
      return std::unexpected(o.error());
    }
    auto parent = decode_revision(*o);
    if (!parent) {
      return std::unexpected(parent.error());
    }
    rev.parents.push_back(p);
    top_generation = std::max(top_generation.value_or(0), parent->generation);
  }
  rev.generation = top_generation ? *top_generation + 1 : 0;
  rev.root = out.root;
  rev.tx = out.tx;
  auto schema_set = schema_set_digest(*schemas_);
  if (!schema_set) {
    return std::unexpected(schema_set.error());
  }
  rev.schema_set = *schema_set;
  auto rev_object = encode_revision(rev);
  if (!rev_object) {
    return std::unexpected(rev_object.error());
  }
  out.revision = rev_object->id;
  out.revision_value = std::move(rev);
  sink.add_always(std::move(*rev_object));

  out.batch.objects = sink.take();
  return out;
}

} // namespace sl::fabric
