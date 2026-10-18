#pragma once

// Helpers for the revision-store tests (WO-046): deterministic ids and digests, a test tx
// record, record types for the patch generator's type pool, commit through the test backdoor,
// and the brute-force oracles the trie is checked against (a map diff and a full walk).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric::testing {

/// `count` distinct derived ids of class `c` (one class and scheme: the worst case for raw-id
/// keys), named "<stream>/<i>" under the project root namespace.
[[nodiscard]] std::vector<record_id> tree_ids(std::size_t count, id_class c = id_class::entity,
                                              std::string_view stream = "tree");

/// SHA-256 of "tree-target/<n>" as a digest: a stand-in object id.
[[nodiscard]] digest tree_target(std::uint64_t n);

/// A record entry naming tree_target(n).
[[nodiscard]] tree_entry record_entry(const record_id &id, std::uint64_t n);

/// A tx record labelled `label` by a well-known actor, with a stand-in patch digest.
[[nodiscard]] gen::fabric_history::tx_record tree_tx(std::string_view label);

/// Revision inputs around tree_tx(label) with tx id well_known(tx, ordinal).
[[nodiscard]] revision_inputs tree_inputs(std::string_view label, std::uint32_t ordinal);

/// Registers "gen.rec0" .. "gen.rec<n-1>" (the patch_gen type pool) as empty record structs of
/// domain "gen", class entity, state document, version 1.<i>.
[[nodiscard]] result<void> add_tree_record_types(schema_registry &reg, std::size_t n = 4);

/// Commits `objects` (no refs, no intents) through testing::backdoor; nothing for an empty list.
[[nodiscard]] result<void> commit_objects(store_backend &store, std::vector<object> objects);

/// Commits `built.batch` through testing::backdoor.
[[nodiscard]] result<commit_ack> commit_built(store_backend &store, const built_revision &built);

/// The oracle diff: the symmetric difference of two id -> entry maps, ascending by id.
[[nodiscard]] std::vector<tree_delta> oracle_diff(const std::map<record_id, tree_entry> &a,
                                                  const std::map<record_id, tree_entry> &b);

/// Every entry of the trie as a map (through tree_reader::entries).
[[nodiscard]] result<std::map<record_id, tree_entry>> trie_map(const tree_reader &reader,
                                                               const digest &root);

/// The longest root-to-leaf path of a trie, in nodes (1 for a single leaf).
[[nodiscard]] result<std::size_t> trie_depth(const tree_reader &reader, const digest &root);

} // namespace sl::fabric::testing
