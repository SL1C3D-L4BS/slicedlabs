// The trie: keys, node codec, object sink and reads (WO-046, SL-ARCH-001 ARCH1-R51, ADR-0014).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint32_t f_bitmap = 1;
constexpr std::uint32_t f_count = 2;
constexpr std::uint32_t f_entries = 3;
constexpr std::uint32_t f_children = 4;
constexpr std::uint32_t e_id = 1;
constexpr std::uint32_t e_kind = 2;
constexpr std::uint32_t e_target = 3;

std::unexpected<error> corrupt(std::string_view what) {
  std::string m("tree node: ");
  m += what;
  return make_error(errc::object_corrupt, std::move(m));
}

bool known_entry_kind(std::uint64_t k) noexcept {
  return k >= static_cast<std::uint64_t>(entry_kind::record) &&
         k <= static_cast<std::uint64_t>(entry_kind::document);
}

result<value> entry_to_value(const tree_entry &e) {
  std::vector<field> fs;
  fs.push_back(field{.no = e_id, .v = value::id(e.id)});
  fs.push_back(field{.no = e_kind, .v = value::uint(static_cast<std::uint64_t>(e.kind))});
  if (e.kind != entry_kind::member) {
    fs.push_back(field{.no = e_target, .v = value::digest(e.target)});
  }
  return value::struct_(std::move(fs));
}

result<tree_entry> entry_from_value(const value &v) {
  if (v.kind() != value_kind::struct_) {
    return corrupt("entry is not a struct");
  }
  const value *id = v.find_field(e_id);
  const value *kind = v.find_field(e_kind);
  const value *target = v.find_field(e_target);
  const std::size_t expected = 2U + (target != nullptr ? 1U : 0U);
  if (id == nullptr || kind == nullptr || v.fields().size() != expected) {
    return corrupt("entry fields are not {1 id, 2 kind, 3 target}");
  }
  if (id->kind() != value_kind::id || kind->kind() != value_kind::uint ||
      !known_entry_kind(kind->as_uint())) {
    return corrupt("entry id or kind malformed");
  }
  auto rid = record_id::from_bytes(id->as_id_bytes());
  if (!rid) {
    return corrupt("entry id is not a valid record id");
  }
  tree_entry e{.id = *rid, .kind = static_cast<entry_kind>(kind->as_uint()), .target = {}};
  const bool wants_target = e.kind != entry_kind::member;
  if (wants_target != (target != nullptr)) {
    return corrupt("entry target present exactly when the kind is not member");
  }
  if (target != nullptr) {
    if (target->kind() != value_kind::digest || target->as_digest().algo != hash_algo::sha256) {
      return corrupt("entry target is not a sha256 digest");
    }
    e.target = target->as_digest();
  }
  return e;
}

digest compute_empty_tree() noexcept {
  auto v = tree_node_to_value(tree_node{});
  if (!v) {
    return digest{};
  }
  auto b = encode(*v);
  if (!b) {
    return digest{};
  }
  return hash_object(frame_header{.kind = object_kind::tree}, *b);
}

} // namespace

trie_key key_of(const record_id &id) noexcept {
  sha256 h;
  h.update(trie_key_domain);
  h.update(byte_span(id.bytes()));
  const sha256::value_type full = h.finish();
  trie_key k{};
  std::copy_n(full.begin(), k.size(), k.begin());
  return k;
}

std::string_view to_string(entry_kind k) noexcept {
  switch (k) {
  case entry_kind::record:
    return "record";
  case entry_kind::tombstone:
    return "tombstone";
  case entry_kind::member:
    return "member";
  case entry_kind::document:
    return "document";
  }
  return "unknown";
}

std::optional<digest> tree_node::child(unsigned n) const noexcept {
  if (n >= 16U || (bitmap & (1U << n)) == 0U) {
    return std::nullopt;
  }
  const auto below = static_cast<std::uint16_t>(bitmap & ((1U << n) - 1U));
  const auto index = static_cast<std::size_t>(std::popcount(below));
  if (index >= children.size()) {
    return std::nullopt;
  }
  return children[index];
}

result<value> tree_node_to_value(const tree_node &n) {
  std::vector<field> fs;
  fs.push_back(field{.no = f_bitmap, .v = value::uint(n.bitmap)});
  fs.push_back(field{.no = f_count, .v = value::uint(n.count)});
  std::vector<value> items;
  if (n.is_leaf()) {
    items.reserve(n.entries.size());
    for (const tree_entry &e : n.entries) {
      auto ev = entry_to_value(e);
      if (!ev) {
        return std::unexpected(ev.error());
      }
      items.push_back(std::move(*ev));
    }
  } else {
    items.reserve(n.children.size());
    for (const digest &d : n.children) {
      items.push_back(value::digest(d));
    }
  }
  auto list = value::list(std::move(items));
  if (!list) {
    return std::unexpected(list.error());
  }
  fs.push_back(field{.no = n.is_leaf() ? f_entries : f_children, .v = std::move(*list)});
  return value::struct_(std::move(fs));
}

result<tree_node> tree_node_from_value(const value &v) {
  if (v.kind() != value_kind::struct_ || v.fields().size() != 3) {
    return corrupt("not a struct of three fields");
  }
  const value *bitmap = v.find_field(f_bitmap);
  const value *count = v.find_field(f_count);
  const value *entries = v.find_field(f_entries);
  const value *children = v.find_field(f_children);
  if (bitmap == nullptr || count == nullptr || bitmap->kind() != value_kind::uint ||
      count->kind() != value_kind::uint || bitmap->as_uint() > 0xFFFFU) {
    return corrupt("bitmap or count malformed");
  }
  tree_node n;
  n.bitmap = static_cast<std::uint16_t>(bitmap->as_uint());
  n.count = count->as_uint();
  if (n.is_leaf()) {
    if (entries == nullptr || entries->kind() != value_kind::list) {
      return corrupt("a leaf holds an entries list");
    }
    n.entries.reserve(entries->items().size());
    n.keys.reserve(entries->items().size());
    for (const value &item : entries->items()) {
      auto e = entry_from_value(item);
      if (!e) {
        return std::unexpected(e.error());
      }
      const trie_key k = key_of(e->id);
      if (!n.entries.empty() && !detail::trie_less(n.keys.back(), n.entries.back().id, k, e->id)) {
        return corrupt("leaf entries not strictly ascending by (key, id)");
      }
      n.entries.push_back(*e);
      n.keys.push_back(k);
    }
    if (n.count != n.entries.size()) {
      return corrupt("leaf count differs from its entries");
    }
    return n;
  }
  if (children == nullptr || children->kind() != value_kind::list) {
    return corrupt("a branch holds a children list");
  }
  if (children->items().size() != static_cast<std::size_t>(std::popcount(n.bitmap))) {
    return corrupt("branch children differ from its bitmap");
  }
  if (n.count <= max_leaf_entries) {
    return corrupt("a branch of 32 or fewer entries");
  }
  n.children.reserve(children->items().size());
  for (const value &item : children->items()) {
    if (item.kind() != value_kind::digest || item.as_digest().algo != hash_algo::sha256) {
      return corrupt("branch child is not a sha256 digest");
    }
    n.children.push_back(item.as_digest());
  }
  return n;
}

result<object> encode_tree_node(const tree_node &n) {
  auto v = tree_node_to_value(n);
  if (!v) {
    return std::unexpected(v.error());
  }
  auto b = encode(*v);
  if (!b) {
    return std::unexpected(b.error());
  }
  return make_object(object_kind::tree, *b);
}

result<tree_node> decode_tree_node(const object &o) {
  if (o.header.kind != object_kind::tree) {
    return corrupt(std::string("object of kind ") + std::string(to_string(o.header.kind)));
  }
  auto v = decode(o.payload);
  if (!v) {
    return corrupt(v.error().message);
  }
  return tree_node_from_value(*v);
}

const digest &empty_tree() noexcept {
  static const digest d = compute_empty_tree();
  return d;
}

result<void> object_sink::add(object o) {
  if (seen_.contains(o.id)) {
    return {};
  }
  if (store_ != nullptr) {
    auto held = store_->has(o.id);
    if (!held) {
      return std::unexpected(held.error());
    }
    if (*held) {
      seen_.insert(o.id);
      return {};
    }
  }
  seen_.insert(o.id);
  objects_.push_back(std::move(o));
  return {};
}

void object_sink::add_always(object o) {
  if (seen_.insert(o.id).second) {
    objects_.push_back(std::move(o));
  }
}

std::vector<object> object_sink::take() noexcept {
  seen_.clear();
  return std::exchange(objects_, {});
}

result<tree_node> tree_reader::load(const digest &id) const {
  if (id == empty_tree()) {
    return tree_node{};
  }
  auto o = store_->get(id);
  if (!o) {
    return std::unexpected(o.error());
  }
  return decode_tree_node(*o);
}

result<tree_node> tree_reader::load_at(const digest &id, std::size_t depth,
                                       const trie_key &prefix) const {
  auto n = load(id);
  if (!n) {
    return n;
  }
  if (!n->is_leaf()) {
    if (depth >= max_trie_depth) {
      return corrupt("a branch below the last key nibble");
    }
    return n;
  }
  if (depth < max_trie_depth && n->entries.size() > max_leaf_entries) {
    return corrupt("a leaf of more than 32 entries above the last key nibble");
  }
  for (const trie_key &k : n->keys) {
    for (std::size_t d = 0; d < depth; ++d) {
      if (key_nibble(k, d) != key_nibble(prefix, d)) {
        return corrupt("a leaf key outside its path");
      }
    }
  }
  return n;
}

result<std::optional<tree_entry>> tree_reader::find(const digest &root, const record_id &id) const {
  const trie_key k = key_of(id);
  digest at = root;
  for (std::size_t depth = 0;; ++depth) {
    auto n = load_at(at, depth, k);
    if (!n) {
      return std::unexpected(n.error());
    }
    if (n->is_leaf()) {
      const auto it =
          std::ranges::find_if(n->entries, [&](const tree_entry &e) { return e.id == id; });
      if (it == n->entries.end()) {
        return std::optional<tree_entry>{};
      }
      return std::optional<tree_entry>{*it};
    }
    const auto next = n->child(key_nibble(k, depth));
    if (!next) {
      return std::optional<tree_entry>{};
    }
    at = *next;
  }
}

result<std::size_t> tree_reader::path_length(const digest &root, const record_id &id) const {
  const trie_key k = key_of(id);
  digest at = root;
  for (std::size_t depth = 0;; ++depth) {
    auto n = load_at(at, depth, k);
    if (!n) {
      return std::unexpected(n.error());
    }
    if (n->is_leaf()) {
      return depth + 1;
    }
    const auto next = n->child(key_nibble(k, depth));
    if (!next) {
      return depth + 1;
    }
    at = *next;
  }
}

result<std::uint64_t> tree_reader::count(const digest &root) const {
  auto n = load(root);
  if (!n) {
    return std::unexpected(n.error());
  }
  return n->count;
}

namespace {

result<std::uint64_t> visit(const tree_reader &reader, const digest &id, std::size_t depth,
                            const trie_key &prefix, const tree_reader::entry_visitor &fn) {
  auto n = reader.load_at(id, depth, prefix);
  if (!n) {
    return std::unexpected(n.error());
  }
  if (n->is_leaf()) {
    if (depth > 0 && n->entries.empty()) {
      return corrupt("an empty child");
    }
    for (const tree_entry &e : n->entries) {
      auto r = fn(e);
      if (!r) {
        return std::unexpected(r.error());
      }
    }
    return n->count;
  }
  std::uint64_t total = 0;
  for (unsigned nib = 0; nib < 16U; ++nib) {
    const auto c = n->child(nib);
    if (!c) {
      continue;
    }
    auto sub = visit(reader, *c, depth + 1, detail::with_nibble(prefix, depth, nib), fn);
    if (!sub) {
      return sub;
    }
    total += *sub;
  }
  if (total != n->count) {
    return corrupt("branch count differs from the sum of its children");
  }
  return total;
}

} // namespace

result<void> tree_reader::for_each(const digest &root, const entry_visitor &fn) const {
  auto r = visit(*this, root, 0, trie_key{}, fn);
  if (!r) {
    return std::unexpected(r.error());
  }
  return {};
}

result<std::vector<tree_entry>> tree_reader::entries(const digest &root) const {
  std::vector<tree_entry> out;
  auto r = for_each(root, [&out](const tree_entry &e) -> result<void> {
    out.push_back(e);
    return {};
  });
  if (!r) {
    return std::unexpected(r.error());
  }
  return out;
}

} // namespace sl::fabric
