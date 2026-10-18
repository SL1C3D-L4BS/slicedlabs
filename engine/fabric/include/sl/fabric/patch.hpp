#pragma once

// Patch v1 (WO-043, SL-ARCH-002 ARCH2-R03, ARCH2-R04, ARCH2-R08, ARCH2-R45, ARCH2-R48; ADR-0018,
// ADR-0019).
//
// A patch is {version, base revision, ordered ops, meta}. Ops come from the closed set of
// SL-ARCH-002 `ops` and address values by field number, map key or element id, never by list
// position (value.hpp paths). Everything here is a pure function over immutable values: a patch
// is applied to a staged_state overlay, never to a stored snapshot.
//
// Expectations (ADR-0018). Every mutating op carries what it expects to find, so its inverse is
// exact and needs no lookup:
//   set, unset       expect: the value at the path, or `absent` when the path held nothing
//   delete_record    captured: the deleted body; type: its type
//   remove_elem      captured: the element value; expect_order_key: its order key
//   move_elem        expect_order_key: the order key before the move
//   retype_record    captured: the body before; expect_type: the type before
// create_record and insert_elem expect their target to be absent, which apply checks. An author
// may leave expectations empty; normalize() fills them from the staged state. A filled
// expectation that does not hold makes apply fail with `conflict`.
//
// Errors of apply, normalize and apply_op: `invalid_argument` for an op missing a part its kind
// needs (or carrying a path where none is allowed, an invalid order key), `not_found` for an
// absent record, parent path or element, `already_exists` for a live record or present element,
// `type_mismatch` for a path that does not fit the value (an element op on a list among them:
// lists are atomic), `conflict` for a filled expectation that does not hold.

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// Patch format version (ADR-0019): a new op kind needs a new minor; from_value rejects a newer
/// major and any op kind it does not know.
inline constexpr std::uint32_t patch_major = 1;
inline constexpr std::uint32_t patch_minor = 0;

/// The closed op set of SL-ARCH-002 `ops`; numbers are stable and stored.
enum class op_kind : std::uint8_t {
  create_record = 1,
  delete_record = 2,
  set = 3,
  unset = 4,
  insert_elem = 5,
  remove_elem = 6,
  move_elem = 7,
  retype_record = 8,
  annotate = 9,
};

/// "create_record", ..., "annotate"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(op_kind k) noexcept;

/// ADR-0018: an irreversible op is a barrier for undo; a compensable op is undone by a registered
/// compensator (WO-052). invert() is mechanical and keeps the mark.
enum class reversibility : std::uint8_t { reversible = 1, compensable = 2, irreversible = 3 };

[[nodiscard]] std::string_view to_string(reversibility r) noexcept;

/// An oset order key: fractional bytes (type.hpp is_valid_order_key).
using order_key = std::vector<std::uint8_t>;

/// What a set or unset expects at its path: unchecked (left for normalize), absent, or a value.
class expectation {
public:
  enum class kind_type : std::uint8_t { unchecked = 0, absent = 1, present = 2 };

  /// Unchecked.
  expectation() noexcept = default;
  [[nodiscard]] static expectation absent() noexcept;
  [[nodiscard]] static expectation of(value v) noexcept;
  /// absent() for nullopt, of(*v) otherwise.
  [[nodiscard]] static expectation of(const std::optional<value> &v) noexcept;

  [[nodiscard]] kind_type kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_unchecked() const noexcept { return kind_ == kind_type::unchecked; }
  [[nodiscard]] bool is_absent() const noexcept { return kind_ == kind_type::absent; }
  [[nodiscard]] bool is_present() const noexcept { return kind_ == kind_type::present; }
  /// The expected value; precondition is_present() (the bool false otherwise).
  [[nodiscard]] const value &expected() const noexcept { return v_; }
  /// True when unchecked, or when `current` is what this expects.
  [[nodiscard]] bool holds(const std::optional<value> &current) const noexcept;
  /// nullopt for absent (and unchecked), the value for present.
  [[nodiscard]] std::optional<value> as_optional() const;

  friend bool operator==(const expectation &a, const expectation &b) noexcept;

private:
  kind_type kind_ = kind_type::unchecked;
  value v_;
};

/// One op. Which parts an op uses (others stay empty):
///   create_record  target, type, new_value (the body)
///   delete_record  target, captured (body), type (its type)
///   set            target, path, new_value, expect
///   unset          target, path (last segment a field or key), expect
///   insert_elem    target, path (the oset), elem, order_key, new_value
///   remove_elem    target, path, elem, captured, expect_order_key
///   move_elem      target, path, elem, order_key, expect_order_key
///   retype_record  target, type (new), new_value (new body), captured, expect_type
///   annotate       new_value (a note; no state change); target optional
struct patch_op {
  op_kind kind = op_kind::annotate;
  record_id target;
  fabric::path path;
  std::optional<value> new_value;
  expectation expect;
  std::optional<value> captured;
  type_id type;
  id_bytes elem{};
  std::vector<std::uint8_t> order_key; // spelled out: the member shares the alias's name
  std::vector<std::uint8_t> expect_order_key;
  type_id expect_type;
  reversibility rev = reversibility::reversible;

  friend bool operator==(const patch_op &, const patch_op &) noexcept = default;
};

struct patch_version {
  std::uint32_t major = patch_major;
  std::uint32_t minor = patch_minor;

  friend constexpr auto operator<=>(const patch_version &,
                                    const patch_version &) noexcept = default;
};

/// Meta (ARCH2-R03): label, undo group, coalesce key, idempotency key, and the transactions an
/// undo reverts (ADR-0018). Empty strings and null ids mean "none".
struct patch_meta {
  std::string label;
  record_id undo_group;
  std::string coalesce_key;
  std::string idempotency_key;
  std::vector<tx_id> undo_of;

  friend bool operator==(const patch_meta &, const patch_meta &) noexcept = default;
};

struct patch {
  patch_version version;
  std::optional<digest> base; // the revision the patch was authored on
  std::vector<patch_op> ops;
  patch_meta meta;

  friend bool operator==(const patch &, const patch &) noexcept = default;
};

// Op constructors (expectations unchecked unless given).
[[nodiscard]] patch_op op_create(const record_id &target, const type_id &type, value body);
[[nodiscard]] patch_op op_delete(const record_id &target);
[[nodiscard]] patch_op op_set(const record_id &target, fabric::path p, value v,
                              expectation expect = {});
[[nodiscard]] patch_op op_unset(const record_id &target, fabric::path p, expectation expect = {});
[[nodiscard]] patch_op op_insert(const record_id &target, fabric::path oset, const id_bytes &elem,
                                 order_key key, value v);
[[nodiscard]] patch_op op_remove(const record_id &target, fabric::path oset, const id_bytes &elem);
[[nodiscard]] patch_op op_move(const record_id &target, fabric::path oset, const id_bytes &elem,
                               order_key key);
[[nodiscard]] patch_op op_retype(const record_id &target, const type_id &type, value body);
[[nodiscard]] patch_op op_annotate(value note);

// ---- State --------------------------------------------------------------------------------

/// A live record as the patch layer sees it: its type and body. Tombstones are not modelled here:
/// a deleted record is absent (the identity validator, WO-058, writes tombstones at commit).
struct record_state {
  type_id type;
  value body;

  friend bool operator==(const record_state &, const record_state &) noexcept = default;
};

/// Read access to records: a snapshot (WO-046), a staged_state, the test model_store.
class state_reader {
public:
  virtual ~state_reader() = default;
  /// The live record `id`, or nullopt when there is none; an error only when reading failed.
  [[nodiscard]] virtual result<std::optional<record_state>> read(const record_id &id) const = 0;

protected:
  state_reader() = default;
  state_reader(const state_reader &) = default;
  state_reader &operator=(const state_reader &) = default;
  state_reader(state_reader &&) = default;
  state_reader &operator=(state_reader &&) = default;
};

/// One record changed by a staged_state: the record after the change, nullopt when removed.
struct staged_change {
  record_id id;
  std::optional<record_state> after;

  friend bool operator==(const staged_change &, const staged_change &) noexcept = default;
};

/// An overlay of changed records over a base reader: a flat vector sorted by record id, so
/// iteration is deterministic. Reads see the overlay first (read-your-writes). The base must
/// outlive the overlay and every copy of it.
class staged_state final : public state_reader {
public:
  explicit staged_state(const state_reader &base) noexcept : base_(&base) {}

  [[nodiscard]] result<std::optional<record_state>> read(const record_id &id) const override;

  /// Applies `op`; on error the state is unchanged.
  [[nodiscard]] result<void> apply(const patch_op &op);
  /// Applies every op in order; on error the state is unchanged (all or nothing).
  [[nodiscard]] result<void> apply(const patch &p);

  /// The changed records, ascending by id. A record changed and changed back stays listed.
  [[nodiscard]] std::span<const staged_change> changes() const noexcept { return changes_; }
  [[nodiscard]] const state_reader &base() const noexcept { return *base_; }

private:
  const state_reader *base_;
  std::vector<staged_change> changes_;
};

/// One op applied to one record (nullopt = absent): the record after it. annotate returns
/// `before` unchanged. Errors as in the file comment.
[[nodiscard]] result<std::optional<record_state>>
apply_op(const std::optional<record_state> &before, const patch_op &op);

/// `op` with every empty expectation filled from `before` (the record just before the op);
/// filled expectations are checked, not replaced. Fails exactly when apply_op fails.
[[nodiscard]] result<patch_op> normalize_op(const std::optional<record_state> &before,
                                            const patch_op &op);

/// ARCH2-R04: the patch with every expectation filled from `state` as each op would see it (ops
/// apply in order on an overlay). Idempotent; a normalised patch is self-inverting.
[[nodiscard]] result<patch> normalize(const patch &p, const state_reader &state);

/// True when every mutating op carries its expectations (what normalize guarantees).
[[nodiscard]] bool is_normalized(const patch &p) noexcept;

// ---- Algebra ------------------------------------------------------------------------------

/// The inverse of a normalised patch: inverted ops in reverse order (SL-ARCH-002 `ops`, column
/// inverse); version, base and meta are kept (the undo manager sets base and undo_of), so
/// invert(invert(p)) == p. An op with an unchecked expectation inverts to an op that cannot
/// restore what it never captured; normalise first (precondition, debug-asserted).
[[nodiscard]] patch invert(const patch &normalized);

/// A patch whose application equals applying `a` then `b` (ARCH2-R22). Ops are concatenated
/// and a set or unset is coalesced into the latest earlier op whose footprint it overlaps when
/// that op is a set of the same record and path: the earlier expectation and the later effect
/// survive (ADR-0018). Ops of `a` coalesce among themselves too, so compose(a, {}) is a's
/// coalesced form, and compose is associative. Keeps a's base and meta; the minor is the larger
/// one.
/// `invalid_argument` for different majors; `conflict` when a coalesced op's filled expectation
/// contradicts what the earlier op leaves (the sequence could never apply).
[[nodiscard]] result<patch> compose(const patch &a, const patch &b);

/// Footprint access modes (SL-ARCH-002 `ops`, column footprint).
enum class access : std::uint8_t { read = 1, write = 2, create = 3, remove = 4 };

[[nodiscard]] std::string_view to_string(access a) noexcept;

struct footprint_entry {
  record_id record;
  fabric::path path;
  access mode = access::read;

  friend bool operator==(const footprint_entry &, const footprint_entry &) noexcept = default;
  /// By record, then path, then mode.
  friend std::strong_ordering operator<=>(const footprint_entry &a,
                                          const footprint_entry &b) noexcept;
};

/// Two entries that conflict: one from each footprint.
struct footprint_overlap {
  footprint_entry first;
  footprint_entry second;

  friend bool operator==(const footprint_overlap &, const footprint_overlap &) noexcept = default;
};

/// What a patch reads and writes (ARCH2-R45): sorted, without duplicates. Two entries conflict
/// when they name the same record, are not both reads, and one path is a prefix of the other
/// (the root path "/" covers the whole record).
struct footprint {
  std::vector<footprint_entry> entries;

  /// Adds an entry, keeping the order and dropping a duplicate.
  void add(footprint_entry e);
  /// Adds every entry of `other`.
  void merge(const footprint &other);
  [[nodiscard]] bool conflicts_with(const footprint &other) const noexcept;
  /// Every conflicting pair, in (first, second) order.
  [[nodiscard]] std::vector<footprint_overlap> overlaps(const footprint &other) const;

  friend bool operator==(const footprint &, const footprint &) noexcept = default;
};

[[nodiscard]] bool conflicts(const footprint_entry &a, const footprint_entry &b) noexcept;

/// The footprint of one op: create(id) and remove(id) cover the record, set and unset write the
/// path, element ops name path/elem, retype writes the record, annotate has none.
[[nodiscard]] footprint footprint_of(const patch_op &op);
[[nodiscard]] footprint footprint_of(const patch &p);

/// True when the footprints do not conflict; symmetric, and sound: both application orders of
/// two patches with commuting footprints give the same state (ADR-0019).
[[nodiscard]] bool commutes(const footprint &a, const footprint &b) noexcept;

/// `p` rebased onto a patch `onto` committed on p's base meanwhile (ARCH2-R08): `p` itself when
/// the footprints are disjoint (its expectations still hold after `onto`), `conflict` naming the
/// first overlap otherwise. The new base revision is set by the caller, which knows it.
[[nodiscard]] result<patch> rebase_if_disjoint(const patch &p, const patch &onto);

// ---- Order keys (ARCH2-R48) ---------------------------------------------------------------

/// A valid key strictly between `lo` and `hi` (an empty bound is open). Equal non-empty bounds
/// return that key: the new element then sorts among its equal-key siblings by element id, which
/// is how concurrent inserts at one key converge. `invalid_argument` for an invalid bound or
/// lo > hi.
[[nodiscard]] result<order_key> between(byte_span lo, byte_span hi);

/// The canonical oset order: order key bytes, then element id.
[[nodiscard]] std::strong_ordering compare_order(byte_span key_a, const id_bytes &elem_a,
                                                 byte_span key_b, const id_bytes &elem_b) noexcept;

// ---- Value form (ARCH2-R49) ---------------------------------------------------------------

/// The patch as a value: struct {1 major, 2 minor, 3 base digest optional, 4 ops list,
/// 5 meta}. op = struct {1 kind, 2 target id, 3 path list, 4 new_value, 5 expect variant
/// {1 absent, 2 value}, 6 captured, 7 type id, 8 elem id, 9 order key bytes, 10 expect order
/// key bytes, 11 rev, 12 expect type id}; a path segment = variant {1 field uint, 2 key, 3
/// element id}; meta = struct {1 label, 2 undo group id, 3 coalesce key, 4 idempotency key, 5
/// undo_of list of ids}. Empty parts are omitted. `encode_invalid` for a string SLB/1 cannot
/// hold.
[[nodiscard]] result<value> to_value(const patch &p);

/// The inverse of to_value. `format_unsupported` for another major or an op kind this reader
/// does not know (whatever the minor); `type_mismatch` for a malformed value, or a field number
/// not in the layout under a minor this reader knows (a newer minor's extra fields are
/// ignored).
[[nodiscard]] result<patch> from_value(const value &v);

} // namespace sl::fabric
