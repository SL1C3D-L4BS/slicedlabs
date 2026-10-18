// FAB-INV-REV-03 (SL-ARCH-001 ARCH1-R53): diff equals the symmetric difference of a map oracle and
// skips equal subtrees. Seeded cases (base sets from empty to several branches deep, from no
// edit to hundreds of inserts, removals, retargets and kind changes, so leaves face branches)
// compare tree_reader::diff in both directions with the brute-force map diff; the skipping cases
// count the nodes read: none for equal roots, exactly the two changed paths for one edit.

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
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

digest put_all(memory_backend &store, const std::map<record_id, tree_entry> &m) {
  std::vector<tree_entry> es;
  es.reserve(m.size());
  for (const auto &[id, e] : m) {
    es.push_back(e);
  }
  object_sink sink(store);
  const subtree t = must(build_tree(es, sink));
  REQUIRE(testing::commit_objects(store, sink.take()).has_value());
  return t.id;
}

std::vector<tree_delta> swapped(std::vector<tree_delta> ds) {
  for (tree_delta &d : ds) {
    std::swap(d.before, d.after);
  }
  return ds;
}

} // namespace

TEST_CASE("FAB-INV-REV-03: diff equals the symmetric difference of a map oracle") {
  const std::vector<std::size_t> base_sizes = {0, 5, 32, 33, 200, 600};
  const std::vector<std::size_t> edit_counts = {0, 1, 2, 8, 40, 300};
  constexpr std::size_t cases = 48;
  memory_backend store;
  const tree_reader reader(store);
  const std::vector<record_id> pool = testing::tree_ids(1000, id_class::entity, "diff");
  for (std::size_t c = 0; c < cases; ++c) {
    testing::rng r(0xD1FF'0000ULL + c);
    const std::size_t n = base_sizes[c % base_sizes.size()];
    const std::size_t edits = edit_counts[(c / base_sizes.size()) % edit_counts.size()];
    std::map<record_id, tree_entry> a;
    for (std::size_t i = 0; i < n; ++i) {
      a[pool[i]] = testing::record_entry(pool[i], i);
    }
    std::map<record_id, tree_entry> b = a;
    std::map<record_id, std::optional<tree_entry>> changes;
    for (std::size_t k = 0; k < edits; ++k) {
      const record_id &id = pool[r.below(pool.size())];
      const auto it = b.find(id);
      std::optional<tree_entry> next;
      if (it == b.end()) {
        next = testing::record_entry(id, 5000 + r.below(100));
      } else if (r.chance(40)) {
        next = std::nullopt;
      } else if (r.chance(50)) {
        next = it->second;
        next->target = testing::tree_target(9000 + r.below(100));
      } else {
        next = it->second;
        next->kind = entry_kind::tombstone;
      }
      if (next) {
        b[id] = *next;
      } else {
        b.erase(id);
      }
      changes[id] = next;
    }
    const digest ra = put_all(store, a);
    std::vector<tree_change> cs;
    cs.reserve(changes.size());
    for (const auto &[id, e] : changes) {
      cs.push_back(tree_change{.id = id, .entry = e});
    }
    object_sink sink(store);
    const digest rb = must(update_tree(reader, ra, std::move(cs), sink)).id;
    REQUIRE(testing::commit_objects(store, sink.take()).has_value());
    CHECK(rb == put_all(store, b));

    const std::vector<tree_delta> expect = testing::oracle_diff(a, b);
    std::vector<tree_delta> forward;
    REQUIRE(reader.diff(ra, rb, forward).has_value());
    std::ranges::sort(forward, {}, &tree_delta::id);
    CHECK_MESSAGE(forward == expect, "case " << c << " n " << n << " edits " << edits);
    std::vector<tree_delta> backward;
    REQUIRE(reader.diff(rb, ra, backward).has_value());
    std::ranges::sort(backward, {}, &tree_delta::id);
    CHECK(backward == swapped(expect));
  }
}

TEST_CASE("FAB-INV-REV-03: diff skips equal subtrees") {
  memory_backend store;
  const tree_reader reader(store);
  const std::vector<record_id> ids = testing::tree_ids(2000, id_class::asset, "skip");
  std::map<record_id, tree_entry> a;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    a[ids[i]] = testing::record_entry(ids[i], i);
  }
  const digest ra = put_all(store, a);

  tree_diff_stats same;
  std::vector<tree_delta> none;
  REQUIRE(reader.diff(ra, ra, none, &same).has_value());
  CHECK(none.empty());
  CHECK(same.nodes_loaded == 0);
  CHECK(same.subtrees_skipped == 1);

  for (std::size_t pick = 0; pick < 8; ++pick) {
    const record_id &id = ids[pick * 211];
    std::map<record_id, tree_entry> b = a;
    b[id].target = testing::tree_target(777'000 + pick);
    const digest rb = put_all(store, b);
    const std::size_t path = must(reader.path_length(ra, id));
    REQUIRE(path >= 3);
    tree_diff_stats stats;
    std::vector<tree_delta> out;
    REQUIRE(reader.diff(ra, rb, out, &stats).has_value());
    REQUIRE(out.size() == 1);
    CHECK(out.front().id == id);
    CHECK(out.front().before == a[id]);
    CHECK(out.front().after == b[id]);
    // Only the two changed paths are read; every sibling subtree on the path is skipped.
    CHECK(stats.nodes_loaded == 2 * path);
    CHECK(stats.subtrees_skipped >= path - 1);
  }
}

TEST_CASE(
    "FAB-INV-REV-03: diff across shapes: a leaf against a branch and against the empty trie") {
  memory_backend store;
  const tree_reader reader(store);
  const std::vector<record_id> ids = testing::tree_ids(300, id_class::entity, "shape");
  std::map<record_id, tree_entry> small;
  std::map<record_id, tree_entry> large;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    large[ids[i]] = testing::record_entry(ids[i], i);
    if (i < 20) {
      small[ids[i]] = testing::record_entry(ids[i], i + (i % 3 == 0 ? 1 : 0));
    }
  }
  const digest rs = put_all(store, small);
  const digest rl = put_all(store, large);
  std::vector<tree_delta> out;
  REQUIRE(reader.diff(rs, rl, out).has_value());
  std::ranges::sort(out, {}, &tree_delta::id);
  CHECK(out == testing::oracle_diff(small, large));
  out.clear();
  REQUIRE(reader.diff(empty_tree(), rl, out).has_value());
  std::ranges::sort(out, {}, &tree_delta::id);
  CHECK(out == testing::oracle_diff({}, large));
}
