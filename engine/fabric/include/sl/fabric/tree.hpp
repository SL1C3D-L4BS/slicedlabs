#pragma once

// The canonical hashed-key trie (WO-046, SL-ARCH-001 ARCH1-R51 and ARCH1-R53, ADR-0014).
//
// Keys: an entry for record id `id` sits under key_of(id) = SHA-256("slf-key/1" || id)[0:16], so
// ids that share class and scheme bytes still spread over the whole fan-out (ARCH1-R51). The
// trie is radix 16: level d branches on nibble d of the key (high nibble of byte 0 first).
//
// Shape: a subtree holding 32 or fewer entries is one leaf bucket, its entries sorted by (key,
// id); a larger subtree is a branch node whose children are the non-empty subtrees of its 16
// nibbles. At depth 32 every key nibble is used up and the node is a leaf whatever its size. The
// shape is therefore a function of the entry set alone: the same entries give the same nodes and
// the same root digest in any insertion order (FAB-INV-REV-01).
//
// Nodes are SLB/1 values framed as object kind tree:
//   struct {1 bitmap uint (u16; 0 for a leaf), 2 count uint (entries in the subtree),
//           3 entries list of struct {1 id id, 2 kind uint, 3 target digest optional} (leaf only),
//           4 children list of digest, one per set bitmap bit in nibble order (branch only)}
// The count field is not in the ARCH node sketch: it lets an update decide whether a branch
// collapses into a leaf without reading all 16 children (deviation recorded for WO-062).
//
// Entry kinds: a record leaf names the record object, a tombstone leaf the tombstone object
// (ARCH1-R05), a document entry of the membership map names the root of the document's member
// trie, and a member entry has no target (the member trie is a set of ids).
//
// The empty trie is the empty leaf. Its digest (empty_tree()) is known without reading, and it is
// never written to a store: readers treat it as empty, and link readers do not report it.
//
// Reads go through a store_backend, which verifies every object id (ARCH1-R11); the reader also
// checks the canonical form of every node it loads (sorted unique entries, counts, bitmap, the
// path prefix of leaf keys) and reports anything else as object_corrupt.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// The first 16 bytes of SHA-256("slf-key/1" || id bytes).
using trie_key = std::array<std::uint8_t, 16>;

/// The domain-separation prefix of trie keys (ARCH1-R51).
inline constexpr std::string_view trie_key_domain = "slf-key/1";
/// A subtree of at most this many entries is one leaf bucket (ARCH1-R51).
inline constexpr std::size_t max_leaf_entries = 32;
/// Nibbles in a key: the deepest level, where every node is a leaf.
inline constexpr std::size_t max_trie_depth = 32;

/// The trie key of `id`.
[[nodiscard]] trie_key key_of(const record_id &id) noexcept;

/// Nibble `depth` (0..31) of `k`: the high nibble of byte depth / 2 for even depths.
[[nodiscard]] constexpr unsigned key_nibble(const trie_key &k, std::size_t depth) noexcept {
  const unsigned b = k[(depth / 2) % k.size()];
  return (depth % 2 == 0) ? (b >> 4U) : (b & 0x0FU);
}

namespace detail {
/// `k` with nibble `depth` set to `n`.
[[nodiscard]] constexpr trie_key with_nibble(trie_key k, std::size_t depth, unsigned n) noexcept {
  std::uint8_t &b = k[(depth / 2) % k.size()];
  const unsigned v = n & 0x0FU;
  const unsigned old = b;
  b = static_cast<std::uint8_t>((depth % 2 == 0) ? ((old & 0x0FU) | (v << 4U))
                                                 : ((old & 0xF0U) | v));
  return k;
}
/// The trie order: by key, then by id (two ids never share a key in practice).
[[nodiscard]] constexpr bool trie_less(const trie_key &ka, const record_id &a, const trie_key &kb,
                                       const record_id &b) noexcept {
  return ka != kb ? ka < kb : a < b;
}
} // namespace detail

/// What a leaf entry names.
enum class entry_kind : std::uint8_t {
  record = 1,    // a record object (kind record)
  tombstone = 2, // a tombstone object (kind tombstone) of a retired id
  member = 3,    // a member id of a document member trie; no target
  document = 4,  // a document of the membership map; the target is its member trie root
};

[[nodiscard]] std::string_view to_string(entry_kind k) noexcept;

/// One leaf entry. `target` is the zero digest for a member entry.
struct tree_entry {
  record_id id;
  entry_kind kind = entry_kind::record;
  digest target;

  friend bool operator==(const tree_entry &, const tree_entry &) noexcept = default;
};

/// A decoded node. A leaf has bitmap 0, its entries sorted by (key, id) and `keys` parallel to
/// them (computed, not stored); a branch has one child digest per set bitmap bit.
struct tree_node {
  std::uint16_t bitmap = 0;
  std::uint64_t count = 0;
  std::vector<tree_entry> entries;
  std::vector<trie_key> keys;
  std::vector<digest> children;

  [[nodiscard]] bool is_leaf() const noexcept { return bitmap == 0; }
  /// The child digest of nibble `n`, or nullopt when that nibble is empty (branches only).
  [[nodiscard]] std::optional<digest> child(unsigned n) const noexcept;
};

/// The SLB/1 value of a node.
[[nodiscard]] result<value> tree_node_to_value(const tree_node &n);
/// The node a value holds; `object_corrupt` unless it has exactly the layout above with a
/// self-consistent shape: a leaf with strictly ascending (key, id) entries, count equal to their
/// number, each target present exactly when the kind is not member; a branch with count above 32
/// and one child per bitmap bit.
[[nodiscard]] result<tree_node> tree_node_from_value(const value &v);
/// The tree object of a node.
[[nodiscard]] result<object> encode_tree_node(const tree_node &n);
/// The node of a tree object; `object_corrupt` for another kind or a malformed node.
[[nodiscard]] result<tree_node> decode_tree_node(const object &o);

/// The digest of the empty leaf (the empty trie).
[[nodiscard]] const digest &empty_tree() noexcept;

/// A subtree written (or already stored): its root digest and entry count.
struct subtree {
  digest id;
  std::uint64_t count = 0;

  friend bool operator==(const subtree &, const subtree &) noexcept = default;
};

/// Collects the new objects of an update in write order, each once. With a store, objects the
/// store already holds are skipped as well, so the batch holds only what is new.
class object_sink {
public:
  object_sink() = default;
  explicit object_sink(const store_backend &dedupe_against) noexcept : store_(&dedupe_against) {}

  /// Adds `o` unless it was added before or the store holds it; the store's error otherwise.
  [[nodiscard]] result<void> add(object o);
  /// Adds `o` unless it was added before (never asks the store).
  void add_always(object o);

  [[nodiscard]] const std::vector<object> &objects() const noexcept { return objects_; }
  [[nodiscard]] std::vector<object> take() noexcept;

private:
  const store_backend *store_ = nullptr;
  std::vector<object> objects_;
  std::set<digest> seen_;
};

/// One change to a trie: put `entry` (whose id must equal `id`), or remove `id` when nullopt.
struct tree_change {
  record_id id;
  std::optional<tree_entry> entry;
};

/// One difference between two tries: the entry in the first and in the second (nullopt when
/// absent there); the two are never equal.
struct tree_delta {
  record_id id;
  std::optional<tree_entry> before;
  std::optional<tree_entry> after;

  friend bool operator==(const tree_delta &, const tree_delta &) noexcept = default;
};

/// What a diff touched: nodes read from the store and pairs of equal subtrees skipped unread.
struct tree_diff_stats {
  std::uint64_t nodes_loaded = 0;
  std::uint64_t subtrees_skipped = 0;
};

/// Read access to tries in a store. Holds a reference: the store must outlive the reader. Every
/// member is const and thread-safe when the store is.
class tree_reader {
public:
  explicit tree_reader(const store_backend &store) noexcept : store_(&store) {}

  using entry_visitor = std::function<result<void>(const tree_entry &)>;

  /// The node `id`, verified (the empty trie without reading).
  [[nodiscard]] result<tree_node> load(const digest &id) const;
  /// load, then the checks of a node found at `depth` under the path whose first `depth` key
  /// nibbles are those of `prefix`: a leaf holds at most 32 entries unless depth is 32, its keys
  /// start with the path, and no branch sits at depth 32 (`object_corrupt` otherwise).
  [[nodiscard]] result<tree_node> load_at(const digest &id, std::size_t depth,
                                          const trie_key &prefix) const;
  /// The entry for `id`, or nullopt.
  [[nodiscard]] result<std::optional<tree_entry>> find(const digest &root,
                                                       const record_id &id) const;
  /// The nodes on the path from the root to the leaf that holds or would hold `id`.
  [[nodiscard]] result<std::size_t> path_length(const digest &root, const record_id &id) const;
  /// The number of entries.
  [[nodiscard]] result<std::uint64_t> count(const digest &root) const;
  /// Calls `fn` for every entry in key order, checking every node on the way; stops at the
  /// first error.
  [[nodiscard]] result<void> for_each(const digest &root, const entry_visitor &fn) const;
  /// Every entry in key order.
  [[nodiscard]] result<std::vector<tree_entry>> entries(const digest &root) const;
  /// Appends the differences from trie `a` to trie `b`, in key order. Equal subtrees are
  /// skipped without being read (ARCH1-R53).
  [[nodiscard]] result<void> diff(const digest &a, const digest &b, std::vector<tree_delta> &out,
                                  tree_diff_stats *stats = nullptr) const;

  [[nodiscard]] const store_backend &store() const noexcept { return *store_; }

private:
  const store_backend *store_;
};

/// Writes the trie of `entries` (any order; ids unique, else `invalid_argument`) into `sink`.
[[nodiscard]] result<subtree> build_tree(std::span<const tree_entry> entries, object_sink &sink);

/// Applies `changes` (ids unique, else `invalid_argument`) to the trie `root` and writes the new
/// nodes into `sink`; untouched subtrees are shared, so a change writes only the nodes on its
/// path. Removing an absent id changes nothing.
[[nodiscard]] result<subtree> update_tree(const tree_reader &reader, const digest &root,
                                          const std::vector<tree_change> &changes,
                                          object_sink &sink);

} // namespace sl::fabric
