#pragma once

// The change-impact engine (WO-056, SL-ARCH-001 ARCH1-R57 to ARCH1-R59; master plan section 2.4
// and section 3, F4 seam and F7).
//
// impact_engine keeps a dependency index of one ref: the dependency graph of WO-044 (deps.hpp),
// the type of every live record and the derivation pin of every derived record (the input ids
// and input record-object digests of the provenance that wrote it). It is a commit observer
// (validate.hpp, step C6): each commit on its ref is applied incrementally from the Merkle diff
// of the parent revision against the new one (snapshot::diff skips equal subtrees), so a commit
// costs O(changed records), never a full rebuild (ARCH1-R57). A full rebuild from a snapshot
// happens only when the engine attaches, when a commit's first parent is not the revision the
// index holds (a reset, a commit the engine did not see) or after an internal error. The
// observer runs after the commit point and never fails a commit: an internal error is logged by
// the manager, and the index is marked for rebuild on the next query or commit.
//
// Derivation pins. Provenance rides on tx records (ARCH1-R45), so the provenance of a derived
// record is that of the tx of the newest first-parent revision at which its records-trie entry
// changed (only complete provenance pins, check_provenance). The incremental path takes it from
// the tx record of the commit; a rebuild walks first-parent history back from the head until
// every live derived record is resolved. Both apply the same rule, so an incremental index
// equals a rebuild from scratch after every commit (FAB-INV-IMP-01).
//
// Impact. impact() is the reverse closure (dependency_graph::reverse_closure): the changed
// records and every record that depends on one of them, ordered by (distance, id), bounded by
// impact_options (max_distance, max_nodes; `truncated` when cut) and by the closure plus its
// frontier edges (FAB-INV-IMP-02). report() groups a closure by id class (summarize(), at most
// `summary_ids` ids per class plus counts) and adds the test and build-target records by type: a
// record with an outgoing `validates` edge is a test, one with an outgoing `builds` edge is a
// build target. Every commit attaches its report to the receipt under impact_options::attachment
// (default `impact_attachment`, as to_value(impact_report)); the receipt's own `impact` field stays
// the manager's distance-0 summary. The committed event's payload is the tx record (WO-050) and the
// engine cannot extend it from an observer, so the report of a committed event is looked up with
// committed(event): observers run before the committed event is published (C6), so a handler of
// "fabric.tx.committed" always finds it (recorded for WO-062).
//
// Currency (F4 seam, ARCH1-R59). is_current(derived, head) is false iff a pinned input's live
// record object in `head` differs from the pinned digest (a deleted or retired input differs);
// a read failure is an error, never `false` or `true` (ADR-0009): an unverifiable artifact is
// never reported current.
//
// The engine is attached explicitly (tests, the WO-062 demo); the workspace does not attach one.
// Observers cannot be removed from a manager, so the manager keeps the engine alive; the manager
// must outlive every call on the engine. Every member is thread-safe.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// The default receipt attachment name of the impact report (impact_options::attachment).
inline constexpr std::string_view impact_attachment = "fabric.impact";

/// The roles of affected records beyond their id class; the numbers are stable.
enum class impact_role : std::uint8_t {
  test = 1,         // a record with an outgoing `validates` edge
  build_target = 2, // a record with an outgoing `builds` edge
};

/// "test", "build_target"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(impact_role r) noexcept;

/// The affected records of one role and one type (the null type for a record that is not live).
struct role_impact {
  impact_role role = impact_role::test;
  type_id type;
  std::uint64_t count = 0;
  std::vector<record_id> first; // the first ids in set order (at most summary_ids)

  friend bool operator==(const role_impact &, const role_impact &) = default;
};

/// An impact set as receipts and events carry it.
struct impact_report {
  /// Counts per id class (asset, entity, instance, derived, ...) and the first ids of each.
  impact_summary summary;
  /// Tests and build targets by type, ascending by (role, type).
  std::vector<role_impact> roles;
  std::uint64_t visited = 0;
  std::uint64_t edges_examined = 0;
  std::optional<digest> at_revision;

  friend bool operator==(const impact_report &, const impact_report &) = default;
};

/// The SLB/1 value of a report: struct {1 total uint, 2 truncated bool, 3 classes list of
/// struct {1 class uint, 2 count uint, 3 ids list of id}, 4 roles list of struct {1 role uint,
/// 2 type id, 3 count uint, 4 ids list of id}, 5 visited uint, 6 edges_examined uint,
/// 7 at_revision digest (absent for none)}.
[[nodiscard]] result<value> to_value(const impact_report &r);
/// The inverse of to_value; `type_mismatch` naming the field for anything else.
[[nodiscard]] result<impact_report> impact_report_from_value(const value &v);

/// One input of a derivation: the input record and the digest of its record object.
struct pinned_input {
  record_id id;
  digest revision;

  friend auto operator<=>(const pinned_input &, const pinned_input &) noexcept = default;
  friend bool operator==(const pinned_input &, const pinned_input &) noexcept = default;
};

/// What a derived record was built from: the record object it pins and its inputs (ascending,
/// unique).
struct derivation_pin {
  digest record_object;
  std::vector<pinned_input> inputs;

  friend bool operator==(const derivation_pin &, const derivation_pin &) = default;
};

/// The pin of a complete provenance record for the record object `record_object`; nullopt when
/// `p` is incomplete (check_provenance).
[[nodiscard]] std::optional<derivation_pin> pin_of(const provenance &p,
                                                   const digest &record_object);

/// The F4 seam over explicit inputs: true iff every input is live in `head` with exactly the
/// pinned record object. A deleted or retired input is `false`; a read failure is the error.
[[nodiscard]] result<bool> is_current(std::span<const pinned_input> inputs, const snapshot &head);

/// The edges one record contributes to the graph.
using edge_source =
    std::function<result<std::vector<edge>>(const record_view &, const schema_registry &)>;

/// The rule of dependency_graph::build for one record: extract_edges, provenance_edges for a
/// derived record and edge_from_record for a record of domain `fabric.edge`. `schema_unknown` for
/// an unregistered type, `invalid_argument` for provenance inputs on a record that is not
/// derived, the extraction errors otherwise.
[[nodiscard]] result<std::vector<edge>> default_edges(const record_view &record,
                                                      const schema_registry &reg);

struct impact_options {
  /// The ref the index follows.
  std::string ref = "main";
  /// The receipt attachment name; engines on one ref need distinct names.
  std::string attachment = std::string(impact_attachment);
  /// Closure bound of impact() and of every commit report (0 is unbounded).
  std::size_t max_nodes = 65536;
  /// Distance bound (nullopt is unbounded).
  std::optional<std::uint32_t> max_distance;
  /// Ids listed per class and per role in a report.
  std::size_t summary_ids = default_summary_ids;
  /// Commit reports kept for committed() (oldest dropped first).
  std::size_t remembered = 256;
  /// Edge extraction; null is default_edges.
  edge_source edges;
};

/// The dependency index of one revision; derived and rebuildable (ARCH1-R57).
struct impact_index {
  dependency_graph graph;
  /// The type of every live record.
  std::map<record_id, type_id> types;
  /// The pin of every live derived record written with complete provenance.
  std::map<record_id, derivation_pin> pins;
  /// The revision the index describes (nullopt for the empty state).
  std::optional<digest> at_revision;

  friend bool operator==(const impact_index &, const impact_index &) = default;
};

/// What the engine did so far.
struct impact_stats {
  std::uint64_t incremental = 0; // commits applied from their diff
  std::uint64_t rebuilds = 0;    // full rebuilds, the one at attach included
  std::uint64_t failures = 0;    // commits whose update failed (index marked for rebuild)
  std::uint64_t deltas = 0;      // records applied incrementally

  friend bool operator==(const impact_stats &, const impact_stats &) noexcept = default;
};

class impact_engine final : public commit_observer {
  struct attach_key {
    explicit attach_key() = default;
  };
  struct state;

public:
  /// Builds the index of `head` and registers the engine as a commit observer of `manager`.
  /// `invalid_argument` when `head` reads another store than the manager's; the rebuild's errors
  /// otherwise. A commit made after `head` was taken makes the next commit rebuild.
  [[nodiscard]] static result<std::shared_ptr<impact_engine>>
  attach(transaction_manager &manager, const snapshot &head, impact_options options = {});

  /// The index of `head` built from scratch (the oracle of FAB-INV-IMP-01).
  [[nodiscard]] static result<impact_index>
  rebuild(const snapshot &head, const schema_registry &reg, const impact_options &options = {});

  impact_engine(attach_key key, std::unique_ptr<state> s) noexcept;
  impact_engine(const impact_engine &) = delete;
  impact_engine(impact_engine &&) = delete;
  impact_engine &operator=(const impact_engine &) = delete;
  impact_engine &operator=(impact_engine &&) = delete;
  ~impact_engine() override;

  /// The reverse closure of `changed` at the indexed revision, bounded by the options.
  /// `invalid_argument` for a null or invalid id; a pending rebuild's errors.
  [[nodiscard]] result<impact_set> impact(std::span<const record_id> changed) const;
  /// impact(changed) grouped by id class and role.
  [[nodiscard]] result<impact_report> report(std::span<const record_id> changed) const;
  /// F4: false iff an input pinned by derived record `derived` differs in `head` (file comment).
  /// `not_found` when `derived` is not live in `head`; `invalid_argument` for a record whose type
  /// is not derived or a head with no revision whose pin the index does not hold;
  /// `provenance_incomplete` when the record has no complete provenance; read errors as they are.
  [[nodiscard]] result<bool> is_current(const record_id &derived, const snapshot &head) const;
  /// The report attached to the receipt of commit `tx`, while remembered.
  [[nodiscard]] std::optional<impact_report> committed(const record_id &tx) const;
  /// The report of a "fabric.tx.committed" event (its causality tx), while remembered.
  [[nodiscard]] std::optional<impact_report> committed(const event_envelope &e) const;
  /// A copy of the index (rebuilt first when marked).
  [[nodiscard]] result<impact_index> index() const;
  [[nodiscard]] impact_stats stats() const;
  /// True after an internal error, until the next query or commit rebuilds.
  [[nodiscard]] bool needs_rebuild() const;

  /// Step C6 (validate.hpp): applies the commit and attaches its report.
  [[nodiscard]] result<void> on_commit(const commit_event &event,
                                       receipt_builder &receipt) override;

private:
  std::unique_ptr<state> state_;
};

} // namespace sl::fabric
