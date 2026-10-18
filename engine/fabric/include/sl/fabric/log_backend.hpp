#pragma once

// The durable store backend (WO-053, SL-ARCH-002 ARCH2-R28 to R31 and R33, ADR-0014, ADR-0015).
//
// A log_backend keeps a `.slfabric` directory through a vfs:
//   FORMAT                     "slfabric 1.0\n", written last when the directory is created
//   log/seg-NNNNNNNN.slg       the commit log (journal.hpp), rotated before 256 MiB
//   loose/ab/<64 hex>          SLFO frames above 1 MiB (written and fsynced before the commit)
// commit() checks the batch exactly like memory_backend, writes loose blobs (tmp file, fsync,
// rename, directory sync), then appends the object frames, the intent frames and the commit frame
// in one append and makes them durable with one fsync (durability::fsync, the default). The
// intact commit frame is the commit point. get() reads through an in-memory oid index built on
// open and verifies every object.
//
// open() makes no recovery decisions (WO-055 does): a torn tail, orphan frames after the last
// commit or a torn segment header in the last segment are `recovery_required`; damage anywhere
// else, an unknown frame type 6-127 or a commit that breaks ARCH2-R30 is `journal_corrupt`.
// A failed append leaves the tail unknown and a failed sync after the append is
// `commit_indeterminate`; either way later commits are `recovery_required` until reopen.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

/// What a commit waits for (ARCH2-R33): nothing, fdatasync, or fsync (the default). Loose blobs
/// and new segments are synced the same way; `none` issues no sync at all.
enum class durability : std::uint8_t { none = 0, flush = 1, fsync = 2 };

struct log_options {
  durability sync = durability::fsync;
  /// A commit that would grow the current segment past this starts a new segment first.
  std::uint64_t rotate_at = segment_rotate_size;
  /// Objects whose SLFO frame is larger than this are stored as loose files.
  std::size_t loose_above = loose_object_threshold;
};

class log_backend final : public store_backend {
  struct open_key {
    explicit open_key() = default;
  };
  struct state;

public:
  /// Opens the `.slfabric` directory `dir` through `fs`, creating it when it does not exist (its
  /// parent must). `fs` must outlive the backend.
  [[nodiscard]] static result<std::unique_ptr<log_backend>>
  open(vfs &fs, const std::filesystem::path &dir, durability sync = durability::fsync) noexcept;
  [[nodiscard]] static result<std::unique_ptr<log_backend>>
  open(vfs &fs, const std::filesystem::path &dir, const log_options &options) noexcept;

  log_backend(open_key key, std::unique_ptr<state> s) noexcept;
  log_backend(const log_backend &) = delete;
  log_backend(log_backend &&) = delete;
  log_backend &operator=(const log_backend &) = delete;
  log_backend &operator=(log_backend &&) = delete;
  ~log_backend() override;

  [[nodiscard]] result<object> get(const digest &id) const noexcept override;
  [[nodiscard]] result<bool> has(const digest &id) const noexcept override;
  [[nodiscard]] result<digest> read_ref(std::string_view name) const noexcept override;
  [[nodiscard]] result<std::vector<ref_entry>> list_refs() const noexcept override;
  [[nodiscard]] result<commit_ack> commit(const mutation_capability &cap,
                                          commit_batch batch) noexcept override;
  [[nodiscard]] result<verify_report> verify(const verify_scope &scope) const noexcept override;

  /// The number of stored objects (framed and loose).
  [[nodiscard]] std::size_t object_count() const noexcept;
  /// The sequence number of the last commit (0 before the first).
  [[nodiscard]] std::uint64_t last_seq() const noexcept;
  /// The SHA-256 of the last commit payload (the next commit's prev); nullopt before the first.
  [[nodiscard]] std::optional<digest> last_commit_digest() const noexcept;
  /// The number of segment files.
  [[nodiscard]] std::uint32_t segment_count() const noexcept;
  /// Every intent record committed, in commit order (an end's kind is empty after reopen).
  [[nodiscard]] std::vector<intent_record> intents() const;
  /// The operations whose begin has no end yet, sorted.
  [[nodiscard]] std::vector<record_id> open_intents() const;
  /// True after a failed append or sync: commits are `recovery_required` until reopen.
  [[nodiscard]] bool needs_recovery() const noexcept;

private:
  std::unique_ptr<state> state_;
};

} // namespace sl::fabric
