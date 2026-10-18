// Merkle-skipping diff of two tries (WO-046, SL-ARCH-001 ARCH1-R53, ADR-0014).
//
// Both sides descend together by key nibble. Two stored subtrees with the same digest hold the
// same entries and are skipped unread. A leaf facing a branch is split by nibble as the branch
// descends, so the two shapes never have to match.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric {

namespace {

// One side of a comparison: a stored subtree not read yet, a loaded branch, or a list of
// entries (a leaf, or the part of a leaf under one nibble).
struct side {
  std::optional<digest> id;
  bool branch = false;
  tree_node node;
  std::vector<tree_entry> entries;
  std::vector<trie_key> keys;

  [[nodiscard]] bool empty_list() const noexcept { return !id && !branch && entries.empty(); }
};

struct diff_walk {
  const tree_reader *reader;
  std::vector<tree_delta> *out;
  tree_diff_stats *stats;

  result<void> expand(side &s, std::size_t depth, const trie_key &prefix) const {
    if (!s.id) {
      return {};
    }
    const digest id = *s.id;
    s.id.reset();
    if (id == empty_tree()) {
      return {};
    }
    auto n = reader->load_at(id, depth, prefix);
    if (!n) {
      return std::unexpected(n.error());
    }
    ++stats->nodes_loaded;
    if (n->is_leaf()) {
      s.entries = std::move(n->entries);
      s.keys = std::move(n->keys);
    } else {
      s.branch = true;
      s.node = std::move(*n);
    }
    return {};
  }

  static side sub(const side &s, unsigned nib, std::size_t depth) {
    side out;
    if (s.branch) {
      out.id = s.node.child(nib);
      return out;
    }
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
      if (key_nibble(s.keys[i], depth) == nib) {
        out.entries.push_back(s.entries[i]);
        out.keys.push_back(s.keys[i]);
      }
    }
    return out;
  }

  void merge(const side &a, const side &b) const {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.entries.size() || j < b.entries.size()) {
      if (j == b.entries.size() ||
          (i < a.entries.size() &&
           detail::trie_less(a.keys[i], a.entries[i].id, b.keys[j], b.entries[j].id))) {
        out->push_back(tree_delta{.id = a.entries[i].id, .before = a.entries[i], .after = {}});
        ++i;
      } else if (i == a.entries.size() || a.entries[i].id != b.entries[j].id) {
        out->push_back(tree_delta{.id = b.entries[j].id, .before = {}, .after = b.entries[j]});
        ++j;
      } else {
        if (a.entries[i] != b.entries[j]) {
          out->push_back(
              tree_delta{.id = a.entries[i].id, .before = a.entries[i], .after = b.entries[j]});
        }
        ++i;
        ++j;
      }
    }
  }

  result<void> walk(side a, side b, std::size_t depth, const trie_key &prefix) const {
    if (a.id && b.id && *a.id == *b.id) {
      ++stats->subtrees_skipped;
      return {};
    }
    auto ea = expand(a, depth, prefix);
    if (!ea) {
      return ea;
    }
    auto eb = expand(b, depth, prefix);
    if (!eb) {
      return eb;
    }
    if (!a.branch && !b.branch) {
      merge(a, b);
      return {};
    }
    for (unsigned nib = 0; nib < 16U; ++nib) {
      side sa = sub(a, nib, depth);
      side sb = sub(b, nib, depth);
      if (sa.empty_list() && sb.empty_list()) {
        continue;
      }
      auto r =
          walk(std::move(sa), std::move(sb), depth + 1, detail::with_nibble(prefix, depth, nib));
      if (!r) {
        return r;
      }
    }
    return {};
  }
};

} // namespace

result<void> tree_reader::diff(const digest &a, const digest &b, std::vector<tree_delta> &out,
                               tree_diff_stats *stats) const {
  tree_diff_stats local;
  const diff_walk w{.reader = this, .out = &out, .stats = stats != nullptr ? stats : &local};
  side sa;
  sa.id = a;
  side sb;
  sb.id = b;
  return w.walk(std::move(sa), std::move(sb), 0, trie_key{});
}

} // namespace sl::fabric
