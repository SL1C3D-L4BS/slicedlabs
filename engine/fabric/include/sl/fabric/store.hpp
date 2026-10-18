#pragma once

// The storage seam (WO-042, SL-ARCH-002 ARCH2-R01, R02 and R07, SL-ARCH-001 ARCH1-R09 and R10,
// ADR-0014, ADR-0016).
//
// A stored object is an SLFO frame: magic `SLFO`, major u8, minor u8, kind u8, flags u8, the LEB128
// payload length, the payload. Its id is hash_object(header, payload) (digest.hpp). A
// store_backend holds objects by id and named refs (heads); it verifies every object on read and
// changes only through commit(), which writes a batch of objects, ref compare-and-swaps and intent
// records all or nothing. commit() takes a `const mutation_capability&`, a passkey only
// `transaction_manager` and the test-only `testing::backdoor` can construct (F5, ADR-0016).

#include <cstddef>
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

namespace sl::fabric {

/// The largest payload a frame may carry (ARCH1-R30, R-SER-LIMIT): 16 MiB.
inline constexpr std::size_t max_object_payload = std::size_t{1} << 24U;

/// The longest ref name, in bytes.
inline constexpr std::size_t max_ref_name = 255;

/// A stored object: its frame header, its payload and its id (the hash of both).
struct object {
  frame_header header;
  std::vector<std::uint8_t> payload;
  digest id;

  /// True when the frame comes from a newer minor version: readable, never written (ARCH1-R10).
  [[nodiscard]] bool read_only() const noexcept { return header.minor > frame_minor; }

  friend bool operator==(const object &, const object &) = default;
};

/// An object of the current frame version (major 1, minor 0, flags 0) with its id computed.
[[nodiscard]] object make_object(object_kind kind, byte_span payload);

/// Parses one SLFO frame; the whole input must be the frame. Checks run in byte order and the
/// first failure wins (the SLB/1 reference, tools/fabric/slb_ref.py parse_frame): bad magic
/// `object_corrupt` (R-SER-FRAME), input shorter than the 8-byte header `decode_truncated`, a major
/// other than 1 `format_unsupported`, a kind outside 1..10 or nonzero flags `object_corrupt`
/// (R-SER-FRAME), a bad length varint `decode_noncanonical` or `decode_truncated`, a payload over
/// 16 MiB `decode_limit`, a short payload `decode_truncated`, trailing bytes
/// `decode_noncanonical`. A newer minor parses; the object is read_only().
[[nodiscard]] result<object> parse_object(byte_span frame) noexcept;

/// The SLFO frame of `payload` under `header` (the payload length is the payload's own).
[[nodiscard]] std::vector<std::uint8_t> frame_object(const frame_header &header, byte_span payload);
/// The SLFO frame of `payload` as major 1, minor 0, flags 0.
[[nodiscard]] std::vector<std::uint8_t> frame_object(object_kind kind, byte_span payload);

/// `format_unsupported` for a major other than 1, `read_only_newer` for a newer minor,
/// `object_corrupt` for an unknown kind or nonzero flags: a store writes only current frames.
[[nodiscard]] result<void> check_writable(const frame_header &header) noexcept;

/// A compare-and-swap on one ref: with `expected` the ref must point there; without it the ref
/// must not exist yet (create-if-absent). Either way it then points at `desired`.
struct ref_update {
  std::string name;
  std::optional<digest> expected;
  digest desired;
};

enum class intent_phase : std::uint8_t { begin = 1, end = 2 };
enum class intent_outcome : std::uint8_t {
  none = 0,
  completed = 1,
  compensated = 2,
  abandoned = 3
};

/// One bracket of a multi-step operation (ARCH2-R24, R36): `begin` with a kind and outcome none,
/// then `end` with the outcome. A begin without an end is an open intent.
struct intent_record {
  record_id operation;
  std::string kind;
  intent_phase phase = intent_phase::begin;
  intent_outcome outcome = intent_outcome::none;

  friend bool operator==(const intent_record &, const intent_record &) = default;
};

/// What one commit writes (ARCH2-R07 step C4). commit_time_us is an unhashed sidecar.
struct commit_batch {
  std::vector<object> objects;
  std::vector<ref_update> refs;
  std::vector<intent_record> intents;
  std::int64_t commit_time_us = 0;
};

/// A successful commit: its sequence number (1 for the first commit of a store, then +1).
struct commit_ack {
  std::uint64_t seq = 0;
};

/// A named head.
struct ref_entry {
  std::string name;
  digest target;

  friend bool operator==(const ref_entry &, const ref_entry &) = default;
};

/// Store-independent checks of a batch, run by every backend before it touches anything:
/// `invalid_argument` for an empty batch, a ref name that is empty, longer than max_ref_name or
/// holds a byte outside 0x21..0x7E, a ref named twice, an intent with a null operation, a begin
/// without a kind or with an outcome, an end without one; check_writable per object;
/// `decode_limit` for a payload over 16 MiB; `object_corrupt` for an object whose id is not the
/// hash of its header and payload.
[[nodiscard]] result<void> check_batch(const commit_batch &batch) noexcept;

/// Lists the object ids a stored object links to, for verify's reachability walk. Supplied by the
/// layer that knows the payload formats (the codec and the revision store).
class link_reader {
public:
  link_reader() = default;
  link_reader(const link_reader &) = delete;
  link_reader(link_reader &&) = delete;
  link_reader &operator=(const link_reader &) = delete;
  link_reader &operator=(link_reader &&) = delete;
  virtual ~link_reader() = default;

  /// Appends the ids `obj` links to onto `out`.
  [[nodiscard]] virtual result<void> append_links(const object &obj,
                                                  std::vector<digest> &out) const noexcept = 0;
};

/// What verify walks: every stored object by hash, and the closure of every ref (through `links`;
/// without one only the ref targets themselves).
struct verify_scope {
  bool hashes = true;
  bool reachability = true;
  const link_reader *links = nullptr;
};

/// One verify finding: `object_corrupt` or `object_missing`, the object id, and the ref whose
/// closure reached it (empty when found by the hash walk).
struct verify_finding {
  errc code = errc::object_corrupt;
  digest id;
  std::string ref;
  std::string message;
};

/// Findings are sorted by (id, code) and name each object once per code.
struct verify_report {
  std::uint64_t objects_hashed = 0;
  std::uint64_t objects_reached = 0;
  std::uint64_t refs_walked = 0;
  std::vector<verify_finding> findings;

  [[nodiscard]] bool ok() const noexcept { return findings.empty(); }
};

class store_backend;

/// The shared verify walk over `store.get` (which verifies each id): the hash walk over `stored`
/// (every object id the backend holds) and the reachability walk from `store.list_refs()`.
[[nodiscard]] result<verify_report> verify_store(const store_backend &store,
                                                 const verify_scope &scope,
                                                 std::span<const digest> stored) noexcept;

class transaction_manager;
class mutation_capability;

namespace testing {
/// Test-only access to the mutation capability (ADR-0016). Declared here and defined only in the
/// test library sl_fabric_testing (engine/fabric/testing/backdoor.cpp): code that is not linked
/// against it cannot call it.
class backdoor {
public:
  backdoor() = delete;

  /// store.commit with a capability made here.
  [[nodiscard]] static result<commit_ack> commit(store_backend &store, commit_batch batch) noexcept;
};
} // namespace testing

/// The passkey of the only mutator (ARCH2-R01, ADR-0016): not copyable, not movable, and
/// constructible only by its friends.
class mutation_capability {
public:
  mutation_capability(const mutation_capability &) = delete;
  mutation_capability(mutation_capability &&) = delete;
  mutation_capability &operator=(const mutation_capability &) = delete;
  mutation_capability &operator=(mutation_capability &&) = delete;
  ~mutation_capability() = default;

private:
  constexpr mutation_capability() noexcept = default;

  friend class transaction_manager;
  friend class testing::backdoor;
};

/// Content-addressed object storage with named refs (ADR-0014). Every member is thread-safe.
class store_backend {
public:
  store_backend() = default;
  store_backend(const store_backend &) = delete;
  store_backend(store_backend &&) = delete;
  store_backend &operator=(const store_backend &) = delete;
  store_backend &operator=(store_backend &&) = delete;
  virtual ~store_backend() = default;

  /// The object with this id, verified: the stored frame is parsed and hashed, and anything but
  /// an exact match is `object_corrupt` with no bytes returned. `object_missing` if absent.
  [[nodiscard]] virtual result<object> get(const digest &id) const noexcept = 0;
  /// Whether an object with this id is stored (unverified).
  [[nodiscard]] virtual result<bool> has(const digest &id) const noexcept = 0;
  /// The target of a ref; `not_found` if there is no such ref.
  [[nodiscard]] virtual result<digest> read_ref(std::string_view name) const noexcept = 0;
  /// Every ref, sorted by name.
  [[nodiscard]] virtual result<std::vector<ref_entry>> list_refs() const noexcept = 0;
  /// Writes `batch` all or nothing: check_batch, then every ref CAS (`ref_cas_failed`), every
  /// ref target present in the store or the batch (`object_missing`), intents consistent with
  /// the open ones (`invalid_argument`); objects already stored are left as they are
  /// (idempotent). On any failure nothing is written and the sequence number is not used.
  [[nodiscard]] virtual result<commit_ack> commit(const mutation_capability &cap,
                                                  commit_batch batch) noexcept = 0;
  /// Hash and reachability walks; findings are data, an error means the walk could not run.
  [[nodiscard]] virtual result<verify_report> verify(const verify_scope &scope) const noexcept = 0;
};

} // namespace sl::fabric
