// FAB-INV-REV-01 (SL-ARCH-001 ARCH1-R51): the trie root depends only on the entry set. Over 200
// seeded permutations, the same entry set is reached by interleaved single and batched updates
// (with overwritten targets and with extra ids inserted and removed again, so leaves split and
// branches collapse) and must end at the root build_tree gives; the golden roots of
// tests/fixtures/fabric/tree/roots.json (computed by an independent Python model) must match.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void flush(memory_backend &store, object_sink &sink) {
  REQUIRE(testing::commit_objects(store, sink.take()).has_value());
}

constexpr std::uint64_t wrong = 1'000'000;

} // namespace

TEST_CASE("FAB-INV-REV-01: trie root depends only on the entry set over 200 permutations") {
  const std::vector<std::size_t> sizes = {20, 33, 40, 56, 72};
  constexpr std::size_t extras = 8;
  constexpr std::size_t permutations = 200;
  memory_backend store;
  const tree_reader reader(store);
  const std::vector<record_id> ids =
      testing::tree_ids(sizes.back() + extras, id_class::entity, "perm");

  std::map<std::size_t, subtree> expected;
  for (const std::size_t n : sizes) {
    std::vector<tree_entry> es;
    es.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      es.push_back(testing::record_entry(ids[i], i));
    }
    object_sink sink;
    expected[n] = must(build_tree(es, sink));
    flush(store, sink);
  }

  for (std::size_t perm = 0; perm < permutations; ++perm) {
    const std::size_t n = sizes[perm % sizes.size()];
    testing::rng r(0x5EED'0000ULL + perm);
    // Per-id op sequences, interleaved at random, each id's ops kept in order.
    std::vector<std::vector<tree_change>> seqs;
    for (std::size_t i = 0; i < n; ++i) {
      std::vector<tree_change> s;
      if (r.chance(20)) {
        s.push_back(tree_change{.id = ids[i], .entry = testing::record_entry(ids[i], wrong + i)});
      }
      s.push_back(tree_change{.id = ids[i], .entry = testing::record_entry(ids[i], i)});
      seqs.push_back(std::move(s));
    }
    for (std::size_t i = n; i < n + extras; ++i) {
      seqs.push_back({tree_change{.id = ids[i], .entry = testing::record_entry(ids[i], i)},
                      tree_change{.id = ids[i], .entry = std::nullopt}});
    }
    std::vector<std::size_t> next(seqs.size(), 0);
    std::vector<std::size_t> open(seqs.size());
    for (std::size_t i = 0; i < open.size(); ++i) {
      open[i] = i;
    }
    digest root = empty_tree();
    while (!open.empty()) {
      const std::size_t batch = r.chance(20) ? 1 : 1 + r.below(32);
      std::vector<tree_change> changes;
      std::set<record_id> in_batch;
      for (std::size_t k = 0; k < batch && !open.empty(); ++k) {
        const std::size_t pick = r.below(open.size());
        const std::size_t s = open[pick];
        const tree_change &c = seqs[s][next[s]];
        if (!in_batch.insert(c.id).second) {
          break;
        }
        changes.push_back(c);
        if (++next[s] == seqs[s].size()) {
          open[pick] = open.back();
          open.pop_back();
        }
      }
      object_sink sink(store);
      root = must(update_tree(reader, root, std::move(changes), sink)).id;
      flush(store, sink);
    }
    CHECK_MESSAGE(root == expected[n].id, "permutation " << perm << " size " << n);
    CHECK(must(reader.count(root)) == n);
  }
}

TEST_CASE("FAB-INV-REV-01: build order does not matter and entries come back in key order") {
  memory_backend store;
  const tree_reader reader(store);
  const std::vector<record_id> ids = testing::tree_ids(500, id_class::instance, "order");
  std::vector<tree_entry> es;
  es.reserve(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    es.push_back(testing::record_entry(ids[i], i));
  }
  object_sink first;
  const subtree a = must(build_tree(es, first));
  flush(store, first);
  testing::rng r(7);
  for (int round = 0; round < 10; ++round) {
    for (std::size_t i = es.size(); i > 1; --i) {
      std::swap(es[i - 1], es[r.below(i)]);
    }
    object_sink sink;
    CHECK(must(build_tree(es, sink)) == a);
  }
  const std::vector<tree_entry> back = must(reader.entries(a.id));
  REQUIRE(back.size() == es.size());
  for (std::size_t i = 1; i < back.size(); ++i) {
    CHECK(key_of(back[i - 1].id) < key_of(back[i].id));
  }
  // A repeated id is refused.
  es.push_back(es.front());
  object_sink sink;
  CHECK(build_tree(es, sink).error().code == sl::errc::invalid_argument);
}

TEST_CASE("FAB-INV-REV-01: golden roots of seeded entry sets") {
  const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/tree/roots.json";
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  const json_value doc = must(parse_json(text.str()));
  const json_value *sets = doc.find("sets");
  REQUIRE(sets != nullptr);
  REQUIRE(sets->items.size() >= 7);
  memory_backend store;
  const tree_reader reader(store);
  for (const json_value &set : sets->items) {
    const std::string name = set.find("name")->text;
    const id_class cls = set.find("class")->text == "asset" ? id_class::asset : id_class::entity;
    const auto count = static_cast<std::size_t>(std::stoull(set.find("count")->text));
    const bool cycle = set.find("kinds")->text == "cycle";
    std::vector<tree_entry> es;
    const std::vector<record_id> ids = testing::tree_ids(count, cls, name);
    for (std::size_t i = 0; i < count; ++i) {
      tree_entry e = testing::record_entry(ids[i], i);
      if (cycle) {
        e.kind = static_cast<entry_kind>((i % 4) + 1);
        if (e.kind == entry_kind::member) {
          e.target = digest{};
        }
      }
      es.push_back(e);
    }
    object_sink sink;
    const subtree t = must(build_tree(es, sink));
    flush(store, sink);
    CHECK_MESSAGE(t.id.to_hex() == set.find("root")->text, name);
    CHECK(t.count == count);
    CHECK(std::to_string(must(testing::trie_depth(reader, t.id))) == set.find("depth")->text);
    if (count > 0) {
      std::vector<tree_entry> back = must(reader.entries(t.id));
      std::ranges::sort(back, {}, &tree_entry::id);
      std::ranges::sort(es, {}, &tree_entry::id);
      CHECK(back == es);
    }
  }
}
