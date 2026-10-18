// Building and updating tries (WO-046, SL-ARCH-001 ARCH1-R51, ADR-0014).
//
// build_range writes the canonical trie of a sorted entry range. update_node rewrites only the
// nodes on the paths of the changes; a changed subtree comes back either in memory (a "small"
// subtree that may still be merged into its parent's leaf) or written (a branch of more than 32
// entries), so no node that a parent collapses is ever written.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric {

namespace {

struct keyed {
  trie_key key{};
  tree_entry entry;
};

struct keyed_change {
  trie_key key{};
  record_id id;
  std::optional<tree_entry> entry;
};

bool less(const keyed &a, const keyed &b) noexcept {
  return detail::trie_less(a.key, a.entry.id, b.key, b.entry.id);
}

tree_entry normalized(tree_entry e) noexcept {
  if (e.kind == entry_kind::member) {
    e.target = digest{};
  }
  return e;
}

result<subtree> write_leaf(std::span<const keyed> es, object_sink &sink) {
  if (es.empty()) {
    return subtree{.id = empty_tree(), .count = 0};
  }
  tree_node n;
  n.count = es.size();
  n.entries.reserve(es.size());
  for (const keyed &k : es) {
    n.entries.push_back(k.entry);
  }
  auto o = encode_tree_node(n);
  if (!o) {
    return std::unexpected(o.error());
  }
  const digest id = o->id;
  auto added = sink.add(std::move(*o));
  if (!added) {
    return std::unexpected(added.error());
  }
  return subtree{.id = id, .count = n.count};
}

// The end of the run of `es` (sorted by key) that shares nibble `depth` with es.front().
std::size_t run_end(std::span<const keyed> es, std::size_t begin, std::size_t depth) noexcept {
  const unsigned nib = key_nibble(es[begin].key, depth);
  std::size_t end = begin + 1;
  while (end < es.size() && key_nibble(es[end].key, depth) == nib) {
    ++end;
  }
  return end;
}

using child_slots = std::vector<std::optional<digest>>; // one per nibble

result<subtree> write_branch(const child_slots &children, std::uint64_t count, object_sink &sink) {
  tree_node n;
  n.count = count;
  for (unsigned nib = 0; nib < children.size(); ++nib) {
    const std::optional<digest> &c = children[nib];
    if (c) {
      n.bitmap = static_cast<std::uint16_t>(n.bitmap | (1U << nib));
      n.children.push_back(*c);
    }
  }
  auto o = encode_tree_node(n);
  if (!o) {
    return std::unexpected(o.error());
  }
  const digest id = o->id;
  auto added = sink.add(std::move(*o));
  if (!added) {
    return std::unexpected(added.error());
  }
  return subtree{.id = id, .count = count};
}

result<subtree> build_range(std::span<const keyed> es, std::size_t depth, object_sink &sink) {
  if (es.size() <= max_leaf_entries || depth >= max_trie_depth) {
    return write_leaf(es, sink);
  }
  child_slots children(16);
  std::size_t begin = 0;
  while (begin < es.size()) {
    const std::size_t end = run_end(es, begin, depth);
    auto child = build_range(es.subspan(begin, end - begin), depth + 1, sink);
    if (!child) {
      return child;
    }
    children[key_nibble(es[begin].key, depth)] = child->id;
    begin = end;
  }
  return write_branch(children, es.size(), sink);
}

// A rewritten subtree: in memory while it may still be merged into a parent leaf, else written.
struct partial {
  bool small = true;
  std::vector<keyed> entries; // small: sorted
  subtree big;                // written: a branch of more than 32 entries

  [[nodiscard]] std::uint64_t count() const noexcept { return small ? entries.size() : big.count; }
};

struct updated {
  partial now;
  std::uint64_t old_count = 0;
};

std::vector<keyed> merge(const tree_node &leaf, std::span<const keyed_change> cs) {
  std::vector<keyed> out;
  out.reserve(leaf.entries.size() + cs.size());
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < leaf.entries.size() || j < cs.size()) {
    const bool take_old = j == cs.size() || (i < leaf.entries.size() &&
                                             detail::trie_less(leaf.keys[i], leaf.entries[i].id,
                                                               cs[j].key, cs[j].id));
    if (take_old) {
      out.push_back(keyed{.key = leaf.keys[i], .entry = leaf.entries[i]});
      ++i;
      continue;
    }
    const keyed_change &c = cs[j];
    const bool same = i < leaf.entries.size() && leaf.entries[i].id == c.id;
    if (c.entry) {
      out.push_back(keyed{.key = c.key, .entry = *c.entry});
    }
    if (same) {
      ++i;
    }
    ++j;
  }
  return out;
}

result<updated> update_node(const tree_reader &reader, const digest &id, std::size_t depth,
                            const trie_key &prefix, std::span<const keyed_change> cs,
                            object_sink &sink) {
  auto loaded = reader.load_at(id, depth, prefix);
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  const tree_node &node = *loaded;
  updated out;
  out.old_count = node.count;
  if (node.is_leaf()) {
    out.now.entries = merge(node, cs);
    if (out.now.entries.size() > max_leaf_entries && depth < max_trie_depth) {
      auto big = build_range(out.now.entries, depth, sink);
      if (!big) {
        return std::unexpected(big.error());
      }
      out.now = partial{.small = false, .entries = {}, .big = *big};
    }
    return out;
  }

  std::vector<std::optional<partial>> changed(16);
  std::uint64_t total = node.count;
  std::size_t begin = 0;
  while (begin < cs.size()) {
    const unsigned nib = key_nibble(cs[begin].key, depth);
    std::size_t end = begin + 1;
    while (end < cs.size() && key_nibble(cs[end].key, depth) == nib) {
      ++end;
    }
    const digest child = node.child(nib).value_or(empty_tree());
    auto sub = update_node(reader, child, depth + 1, detail::with_nibble(prefix, depth, nib),
                           cs.subspan(begin, end - begin), sink);
    if (!sub) {
      return sub;
    }
    total = total - sub->old_count + sub->now.count();
    changed[nib] = std::move(sub->now);
    begin = end;
  }

  if (total <= max_leaf_entries) {
    // Collapse: every child now holds at most 32 entries, so each is a leaf.
    for (unsigned nib = 0; nib < 16U; ++nib) {
      std::optional<partial> &ch = changed[nib];
      if (ch) {
        if (!ch->small) {
          return make_error(errc::object_corrupt, "tree node: branch count inconsistent");
        }
        std::ranges::move(ch->entries, std::back_inserter(out.now.entries));
        continue;
      }
      const auto c = node.child(nib);
      if (!c) {
        continue;
      }
      auto leaf = reader.load_at(*c, depth + 1, detail::with_nibble(prefix, depth, nib));
      if (!leaf) {
        return std::unexpected(leaf.error());
      }
      if (!leaf->is_leaf()) {
        return make_error(errc::object_corrupt, "tree node: branch count inconsistent");
      }
      for (std::size_t i = 0; i < leaf->entries.size(); ++i) {
        out.now.entries.push_back(keyed{.key = leaf->keys[i], .entry = leaf->entries[i]});
      }
    }
    if (out.now.entries.size() != total) {
      return make_error(errc::object_corrupt, "tree node: branch count inconsistent");
    }
    return out;
  }

  child_slots children(16);
  for (unsigned nib = 0; nib < 16U; ++nib) {
    const std::optional<partial> &ch = changed[nib];
    if (!ch) {
      children[nib] = node.child(nib);
      continue;
    }
    const partial &p = *ch;
    if (!p.small) {
      children[nib] = p.big.id;
    } else if (!p.entries.empty()) {
      auto leaf = write_leaf(p.entries, sink);
      if (!leaf) {
        return std::unexpected(leaf.error());
      }
      children[nib] = leaf->id;
    }
  }
  auto branch = write_branch(children, total, sink);
  if (!branch) {
    return std::unexpected(branch.error());
  }
  out.now = partial{.small = false, .entries = {}, .big = *branch};
  return out;
}

} // namespace

result<subtree> build_tree(std::span<const tree_entry> entries, object_sink &sink) {
  std::vector<keyed> es;
  es.reserve(entries.size());
  for (const tree_entry &e : entries) {
    if (e.id.is_null()) {
      return make_error(errc::invalid_argument, "trie entry with the null id");
    }
    const trie_key k = key_of(e.id);
    es.push_back(keyed{.key = k, .entry = normalized(e)});
  }
  std::ranges::sort(es, less);
  for (std::size_t i = 1; i < es.size(); ++i) {
    if (es[i].entry.id == es[i - 1].entry.id) {
      return make_error(errc::invalid_argument,
                        "trie entry id given twice: " + es[i].entry.id.to_string());
    }
  }
  return build_range(es, 0, sink);
}

result<subtree> update_tree(const tree_reader &reader, const digest &root,
                            std::vector<tree_change> changes, object_sink &sink) {
  std::vector<keyed_change> cs;
  cs.reserve(changes.size());
  for (tree_change &c : changes) {
    if (c.id.is_null() || (c.entry && c.entry->id != c.id)) {
      return make_error(errc::invalid_argument, "trie change with a null or mismatched id");
    }
    std::optional<tree_entry> e;
    if (c.entry) {
      e = normalized(*c.entry);
    }
    cs.push_back(keyed_change{.key = key_of(c.id), .id = c.id, .entry = e});
  }
  std::ranges::sort(cs, [](const keyed_change &a, const keyed_change &b) {
    return detail::trie_less(a.key, a.id, b.key, b.id);
  });
  for (std::size_t i = 1; i < cs.size(); ++i) {
    if (cs[i].id == cs[i - 1].id) {
      return make_error(errc::invalid_argument,
                        "trie change id given twice: " + cs[i].id.to_string());
    }
  }
  if (cs.empty()) {
    auto n = reader.count(root);
    if (!n) {
      return std::unexpected(n.error());
    }
    return subtree{.id = root, .count = *n};
  }
  auto top = update_node(reader, root, 0, trie_key{}, cs, sink);
  if (!top) {
    return std::unexpected(top.error());
  }
  if (!top->now.small) {
    return top->now.big;
  }
  return write_leaf(top->now.entries, sink);
}

} // namespace sl::fabric
