// Helpers for the revision-store tests (WO-046).

#include "tree_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/gen/fabric.provenance.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric::testing {

std::vector<record_id> tree_ids(std::size_t count, id_class c, std::string_view stream) {
  const record_id root = well_known(id_class::namespace_, 1);
  std::vector<record_id> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(derive(c, root, std::format("{}/{}", stream, i)));
  }
  return out;
}

digest tree_target(std::uint64_t n) {
  digest d;
  d.bytes = sha256::hash(std::format("tree-target/{}", n));
  return d;
}

tree_entry record_entry(const record_id &id, std::uint64_t n) {
  return tree_entry{.id = id, .kind = entry_kind::record, .target = tree_target(n)};
}

gen::fabric_history::tx_record tree_tx(std::string_view label) {
  gen::fabric_history::tx_record tx;
  tx.actor = well_known(id_class::actor, 1);
  tx.origin.subsystem = "02";
  tx.origin.epistemic = gen::fabric_provenance::epistemic_class::asserted;
  tx.patch = tree_target(0xFFFF'FFFFULL);
  tx.label = std::string(label);
  return tx;
}

revision_inputs tree_inputs(std::string_view label, std::uint32_t ordinal) {
  return revision_inputs{
      .tx = tree_tx(label), .tx_id = well_known(id_class::tx, ordinal), .extra_parents = {}};
}

result<void> add_tree_record_types(schema_registry &reg, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    type_desc t;
    t.name = std::format("gen.rec{}", i);
    t.id = type_id_of(t.name);
    t.kind = type_kind::struct_;
    t.version = type_version{.major = 1, .minor = static_cast<std::uint32_t>(i)};
    t.domain = "gen";
    t.record_class = id_class::entity;
    t.state_class = state_class::document;
    auto r = reg.add(std::move(t));
    if (!r) {
      return r;
    }
  }
  return {};
}

result<void> commit_objects(store_backend &store, std::vector<object> objects) {
  if (objects.empty()) {
    return {};
  }
  commit_batch batch;
  batch.objects = std::move(objects);
  auto ack = backdoor::commit(store, std::move(batch));
  if (!ack) {
    return std::unexpected(ack.error());
  }
  return {};
}

result<commit_ack> commit_built(store_backend &store, const built_revision &built) {
  return backdoor::commit(store, built.batch);
}

std::vector<tree_delta> oracle_diff(const std::map<record_id, tree_entry> &a,
                                    const std::map<record_id, tree_entry> &b) {
  std::vector<tree_delta> out;
  for (const auto &[id, e] : a) {
    const auto it = b.find(id);
    if (it == b.end()) {
      out.push_back(tree_delta{.id = id, .before = e, .after = {}});
    } else if (it->second != e) {
      out.push_back(tree_delta{.id = id, .before = e, .after = it->second});
    }
  }
  for (const auto &[id, e] : b) {
    if (!a.contains(id)) {
      out.push_back(tree_delta{.id = id, .before = {}, .after = e});
    }
  }
  std::ranges::sort(out, {}, &tree_delta::id);
  return out;
}

result<std::map<record_id, tree_entry>> trie_map(const tree_reader &reader, const digest &root) {
  auto es = reader.entries(root);
  if (!es) {
    return std::unexpected(es.error());
  }
  std::map<record_id, tree_entry> out;
  for (const tree_entry &e : *es) {
    out.emplace(e.id, e);
  }
  return out;
}

namespace {

result<std::size_t> depth_below(const tree_reader &reader, const digest &id) {
  auto n = reader.load(id);
  if (!n) {
    return std::unexpected(n.error());
  }
  std::size_t deepest = 0;
  for (const digest &c : n->children) {
    auto d = depth_below(reader, c);
    if (!d) {
      return d;
    }
    deepest = std::max(deepest, *d);
  }
  return deepest + 1;
}

} // namespace

result<std::size_t> trie_depth(const tree_reader &reader, const digest &root) {
  return depth_below(reader, root);
}

} // namespace sl::fabric::testing
