#pragma once

// The dependency graph (WO-044, SL-ARCH-001 ARCH1-R55 to ARCH1-R58, master plan F7).
//
// An edge `from -> to` reads "from depends on to": a record with a strong ref to `to`, an owner of
// `to` (owned ref), a derived record built from the input `to`, or a record named by a
// `fabric.edge` record. Edges come from exactly four places (ARCH1-R55): strong refs (kind
// reference), owned refs (kind containment), the provenance inputs of a derived record (kind
// derives_from) and explicit edge records of domain `fabric.edge`, whose kind is any of the five. A
// weak ref makes no edge.
//
// Reference cycles are legal and condensed into strongly connected components (Tarjan);
// containment, derives_from and builds must each be acyclic (ARCH1-R56), and a cycle is
// `dependency_cycle` with the witness path in the message.
//
// The change impact of a set of changed records is their reverse closure: the changed records and
// every record that depends on one of them, directly or through a chain. It is ordered by
// (distance, id), so it is the same set in the same order on every platform (ARCH1-R58).
//
// Everything iterates in record id order. The graph reads records through `record_enumerator`
// (snapshots implement it in WO-046, the tests use testing/deps_model_store.hpp) and never holds a
// snapshot: it is derived and rebuildable (ARCH1-R57), and `set_record` / `erase_record` are the
// seam of the incremental index of WO-056.
//
// Edge record layout (the fabric.edge schema is not authored yet): the body is a struct with field
// 1 `from` (an id), field 2 `to` (an id) and field 3 `kind` (a uint, the edge_kind number); other
// fields are ignored.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// Edge kinds (ARCH1-R55); the numbers are stable.
enum class edge_kind : std::uint8_t {
  reference = 1,    // a strong ref
  containment = 2,  // an owned ref: the owner depends on what it owns
  derives_from = 3, // a derived record and one input of its provenance
  builds = 4,       // an edge record: a build target and what it builds from
  validates = 5,    // an edge record: a test and what it validates
};

[[nodiscard]] std::string_view to_string(edge_kind k) noexcept;
[[nodiscard]] constexpr bool is_valid(edge_kind k) noexcept {
  const auto n = static_cast<std::uint8_t>(k);
  return n >= 1 && n <= 5;
}

/// A set of edge kinds, one bit per kind (bit k-1).
using edge_kind_mask = std::uint32_t;
[[nodiscard]] constexpr edge_kind_mask mask_of(edge_kind k) noexcept {
  return is_valid(k) ? (edge_kind_mask{1} << (static_cast<std::uint8_t>(k) - 1)) : 0;
}
[[nodiscard]] constexpr bool contains(edge_kind_mask m, edge_kind k) noexcept {
  return (m & mask_of(k)) != 0;
}
inline constexpr edge_kind_mask all_edge_kinds =
    mask_of(edge_kind::reference) | mask_of(edge_kind::containment) |
    mask_of(edge_kind::derives_from) | mask_of(edge_kind::builds) | mask_of(edge_kind::validates);
/// The kinds that must be acyclic (ARCH1-R56).
inline constexpr edge_kind_mask acyclic_edge_kinds =
    mask_of(edge_kind::containment) | mask_of(edge_kind::derives_from) | mask_of(edge_kind::builds);

/// `from` depends on `to`. strength is the ref strength for reference and containment edges and
/// strong for every other kind.
struct edge {
  record_id from;
  record_id to;
  edge_kind kind = edge_kind::reference;
  ref_strength strength = ref_strength::strong;

  friend constexpr auto operator<=>(const edge &, const edge &) noexcept = default;
};

/// The edges a record body holds through its strong and owned refs, sorted and without
/// duplicates. Refs are found type-directed, through struct fields, variant payloads, tagged
/// bases, list items, map values and oset elements (ARCH1-R18). `type_mismatch` names the path of
/// a value that does not fit its type, of an undeclared field or of a ref that is not a valid id;
/// `schema_unknown` a type the registry does not hold.
[[nodiscard]] result<std::vector<edge>> extract_edges(const record_id &from, const value &body,
                                                      const type_desc &type,
                                                      const schema_registry &reg);

/// One derives_from edge per input, sorted and without duplicates.
[[nodiscard]] std::vector<edge> provenance_edges(const record_id &from,
                                                 std::span<const record_id> inputs);

/// The edge a `fabric.edge` record body names (layout in the file comment): `type_mismatch` for a
/// body that is not a struct or misses a field, `id_parse` for an invalid id, `invalid_argument`
/// for an edge kind outside 1..5.
[[nodiscard]] result<edge> edge_from_record(const value &body);

/// One record as the graph reads it. `body` is never null. `provenance_inputs` are the input ids
/// of a derived record (empty otherwise).
struct record_view {
  record_id id;
  type_id type;
  const value *body = nullptr;
  std::span<const record_id> provenance_inputs;
};

/// A source of records: implemented by snapshots (WO-046) and by testing/deps_model_store.hpp.
class record_enumerator {
public:
  using visitor = std::function<result<void>(const record_view &)>;

  virtual ~record_enumerator() = default;
  /// Calls `fn` once per record, in ascending id order, and stops at the first error `fn` returns
  /// (returned unchanged) or at the first read failure.
  [[nodiscard]] virtual result<void> for_each(const visitor &fn) const = 0;

protected:
  record_enumerator() = default;
  record_enumerator(const record_enumerator &) = default;
  record_enumerator &operator=(const record_enumerator &) = default;
  record_enumerator(record_enumerator &&) = default;
  record_enumerator &operator=(record_enumerator &&) = default;
};

/// A strongly connected component of the graph.
struct scc_component {
  std::vector<record_id> members; // ascending
  bool cyclic = false;            // more than one member, or a member with an edge to itself

  friend bool operator==(const scc_component &, const scc_component &) = default;
};

/// The components of a graph and the DAG between them (Tarjan, iterative). Components are
/// numbered in ascending order of their smallest member.
struct condensation {
  std::vector<scc_component> components;
  /// Component index of every node.
  std::map<record_id, std::size_t> component_of;
  /// (from component, to component): `from` depends on `to`; ascending, no duplicates, no loops.
  std::vector<std::pair<std::size_t, std::size_t>> edges;
  /// Component indexes, every component after the components it depends on; ties go to the
  /// smaller index.
  std::vector<std::size_t> order;
};

struct closure_options {
  /// Only edges of these kinds are followed.
  edge_kind_mask kinds = all_edge_kinds;
  /// Records farther than this are left out (0 follows no edge); nullopt is unbounded.
  std::optional<std::uint32_t> max_distance;
  /// Keep only the first N nodes in (distance, id) order and set `truncated` (0 is unbounded).
  std::size_t max_nodes = 0;
  /// Copied into impact_set::at_revision.
  std::optional<digest> at_revision;
};

/// One affected record. `via` is the record one step closer to the changed set and `via_kind` the
/// kind of the edge `id -> via` that was followed; among several candidates the smallest via, then
/// the smallest kind. Changed records have distance 0 and a null via.
struct impact_node {
  record_id id;
  id_class cls = id_class::none;
  std::uint32_t distance = 0;
  record_id via;
  edge_kind via_kind = edge_kind::reference;

  friend bool operator==(const impact_node &, const impact_node &) = default;
};

/// The reverse closure of a changed set, ordered by (distance, id).
struct impact_set {
  std::vector<impact_node> nodes;
  /// Nodes whose dependents were looked up (a node at max_distance is not expanded).
  std::size_t visited = 0;
  /// Edges looked at while expanding: at most the edges that end in a closure node.
  std::size_t edges_examined = 0;
  /// True when max_nodes cut the set.
  bool truncated = false;
  std::optional<digest> at_revision;

  friend bool operator==(const impact_set &, const impact_set &) = default;
};

/// The affected records of one id class.
struct class_impact {
  id_class cls = id_class::none;
  std::uint64_t count = 0;
  std::vector<record_id> first; // the first ids of the class in set order (at most `first_n`)

  friend bool operator==(const class_impact &, const class_impact &) = default;
};

/// An impact set grouped by id class: the shape on commit receipts and events (WO-050, WO-056).
struct impact_summary {
  std::vector<class_impact> classes; // ascending class, only classes with a node
  std::uint64_t total = 0;
  bool truncated = false;

  friend bool operator==(const impact_summary &, const impact_summary &) = default;
};

inline constexpr std::size_t default_summary_ids = 64;

/// Counts per id class plus the first `first_n` ids of each class in (distance, id) order.
[[nodiscard]] impact_summary summarize(const impact_set &set,
                                       std::size_t first_n = default_summary_ids);

class dependency_graph {
public:
  /// Reads every record of `records` and builds the graph: extract_edges, provenance_edges for a
  /// record whose type is a derived state class, and edge_from_record for a record whose type
  /// domain is `fabric.edge`. `schema_unknown` for a type missing from `reg`, `already_exists`
  /// for a record id enumerated twice, `invalid_argument` for provenance inputs on a record that
  /// is not derived; the first error of the walk otherwise.
  [[nodiscard]] static result<dependency_graph> build(const record_enumerator &records,
                                                      const schema_registry &reg);

  /// Replaces everything record `owner` contributes to the graph. An edge starts at the owner for
  /// strong and owned refs and provenance inputs, and at the named `from` for a `fabric.edge`
  /// record. An edge several owners contribute is one edge until the last of them drops it.
  /// `invalid_argument` unless every edge has a valid from, to and kind. Edges are sorted and
  /// de-duplicated.
  [[nodiscard]] result<void> set_record(const record_id &owner, std::vector<edge> edges);
  /// Drops everything `owner` contributes and marks it absent; edges that end in it stay (they
  /// dangle).
  void erase_record(const record_id &owner);

  /// True for an owner set with set_record and not erased.
  [[nodiscard]] bool contains_record(const record_id &id) const noexcept;
  /// Every record id the graph knows: present records and edge targets, ascending.
  [[nodiscard]] std::vector<record_id> nodes() const;
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return counts_.size(); }
  /// Every edge, ascending.
  [[nodiscard]] std::vector<edge> edges() const;

  /// Edges that start at `id`, ascending (empty for an unknown id).
  [[nodiscard]] std::span<const edge> out(const record_id &id) const noexcept;
  /// Edges that end at `id`, ascending by (from, kind) (empty for an unknown id).
  [[nodiscard]] std::span<const edge> in(const record_id &id) const noexcept;

  /// The shortest cycle through the smallest id that lies on a cycle of the first cyclic
  /// component, over the edges whose kind is in `kinds`, as the edges around the cycle; nullopt
  /// when those edges are acyclic.
  [[nodiscard]] std::optional<std::vector<edge>> find_cycle(edge_kind_mask kinds) const;

  /// `dependency_cycle` for the first kind in `kinds` (ascending kind number) whose edges alone
  /// contain a cycle; the message is "<kind> cycle: <id> -> <id> -> ... -> <id>", the witness of
  /// find_cycle, starting and ending at the same id. Each kind is judged on its own: a
  /// containment edge and a derives_from edge never close a cycle together.
  [[nodiscard]] result<void> check_acyclic(edge_kind_mask kinds = acyclic_edge_kinds) const;

  /// Tarjan condensation over the edges whose kind is in `kinds`.
  [[nodiscard]] condensation condense(edge_kind_mask kinds = all_edge_kinds) const;

  /// The changed records and everything that depends on them, ordered by (distance, id).
  /// `invalid_argument` for a null or invalid changed id. A changed id the graph does not know is
  /// still in the set (a deleted record's dependents are affected).
  [[nodiscard]] result<impact_set> reverse_closure(std::span<const record_id> changed,
                                                   const closure_options &options = {}) const;

  friend bool operator==(const dependency_graph &a, const dependency_graph &b) noexcept;

private:
  struct node {
    std::vector<edge> out;
    std::vector<edge> in;
    bool present = false;

    friend bool operator==(const node &, const node &) = default;
  };

  void acquire(const edge &e);
  void release(const edge &e);
  void drop_if_unused(const record_id &id);

  std::map<record_id, node> nodes_;
  /// What each owner contributes (sorted, unique) and how many owners contribute each edge.
  std::map<record_id, std::vector<edge>> owners_;
  std::map<edge, std::uint32_t> counts_;
};

} // namespace sl::fabric
