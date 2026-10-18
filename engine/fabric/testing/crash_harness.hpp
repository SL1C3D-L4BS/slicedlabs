#pragma once

// The crash harness (WO-055, SL-ARCH-002 ARCH2-R35 to R40, FAB-INV-RCV-01, -02, -09): seeded
// workloads over the patch generator with the oracle state and the head after every commit (from
// a fault-free memory_backend run), a durable workspace on a faulty vfs that can be killed after
// any storage primitive or at any commit failpoint and then crashed under a policy, and the check
// that a reopen with recovery loses no acknowledged commit, never shows a partial one, is
// idempotent, and leaves a writable workspace that finishes the workload on the oracle.
// Also: disk images of a vfs_memory (with empty directories) and their restoration, the frame
// table of a segment, SL_FABRIC_SWEEP_SCALE, SL_FABRIC_SOAK_SEED, and a scratch directory under
// the ctest working directory for tests on the posix vfs.
// Include as "../../../engine/fabric/testing/crash_harness.hpp" from tests.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "faulty_vfs.hpp"
#include "model_store.hpp"
#include "patch_gen.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric::testing {

/// SL_FABRIC_SWEEP_SCALE: a positive number scaling every sweep (default 1; the sanitizer presets
/// set 0.1).
[[nodiscard]] double sweep_scale() noexcept;
/// max(minimum, n * sweep_scale()), rounded to nearest.
[[nodiscard]] std::size_t scaled(std::size_t n, std::size_t minimum = 1) noexcept;
/// SL_FABRIC_SOAK_SEED when set to a number (`just fabric-soak`), else nullopt.
[[nodiscard]] std::optional<std::uint64_t> soak_seed() noexcept;

/// The worker threads of a sweep: the hardware threads, 1 to 8.
[[nodiscard]] std::size_t sweep_threads() noexcept;
/// Runs body(0) .. body(count - 1) on sweep_threads() threads. `body` must be thread-safe and must
/// not use doctest assertions (collect results, assert on the calling thread).
void parallel_for(std::size_t count, const std::function<void(std::size_t)> &body);

/// The workspace directory every harness workspace lives in.
[[nodiscard]] std::filesystem::path harness_root();
/// harness_root() / ".slfabric".
[[nodiscard]] std::filesystem::path harness_store();

/// A seeded workload: commit 1 loads a generated state, the others are generated patches; every
/// patch carries the idempotency key "w-<index>".
struct crash_workload {
  std::uint64_t seed = 0;
  std::vector<patch> patches;               ///< patch i is commit i + 1
  std::vector<std::optional<digest>> heads; ///< heads[k]: the main head after k commits
  std::vector<oracle_store> states;         ///< states[k]: the records after k commits
};

/// A workload of `commits` commits (at least 1) over `records` generated records, at most
/// `max_ops` ops per generated patch.
[[nodiscard]] result<crash_workload> make_workload(std::uint64_t seed, std::size_t commits,
                                                   std::size_t records = 4,
                                                   std::size_t max_ops = 4);

/// The durable workspace of the harness: small segments and a low loose threshold make a short
/// workload rotate segments and write loose blobs.
struct harness_config {
  log_options log{.sync = durability::fsync, .rotate_at = 4096, .loose_above = 512};
  recovery_policy policy;
};

/// Workspace options over the generator's types on `fs`, with `cfg.log`.
[[nodiscard]] result<workspace_options> harness_options(std::shared_ptr<vfs> fs,
                                                        const harness_config &cfg);

/// The origin of every harness commit.
[[nodiscard]] tx_options harness_tx();

/// What one run of the workload did before it stopped.
struct crash_run {
  bool created = false;   ///< workspace::create returned
  std::size_t acked = 0;  ///< commits acknowledged
  bool in_flight = false; ///< commit() was running when the run stopped
  std::optional<error> failure;
  std::uint64_t primitives = 0;     ///< counted vfs primitives of the run
  std::string failed_primitive;     ///< the trace line of the injected failure ("" if none)
  std::optional<failpoint> tripped; ///< the failpoint that stopped the run
  /// The injected failure hit a primitive that changes nothing (read, size, list, exists): the
  /// disk is as it was after the previous primitive.
  bool failed_on_read = false;
  /// The last append before the failure was not synced: tear_last_append differs from
  /// drop_unsynced.
  bool unsynced_append = false;
};

/// Fills failed_primitive, failed_on_read and unsynced_append of `run` from a faulty vfs trace.
void note_failure(const std::vector<std::string> &trace, crash_run &run);

/// A failpoint armed on one commit (0-based).
struct armed_failpoint {
  std::size_t commit = 0;
  failpoint fp = failpoint::fp0;
};

/// Creates the workspace in harness_root() on `fs`, then commits the workload until it is done or
/// the first error (the process is taken to die there). `fail_after`: the vfs fails that primitive
/// (counted from the start of the run) and every later one. The workspace is closed at the end.
[[nodiscard]] crash_run run_workload(const std::shared_ptr<faulty_vfs> &fs, const crash_workload &w,
                                     const harness_config &cfg,
                                     std::optional<std::uint64_t> fail_after,
                                     std::optional<armed_failpoint> fp = std::nullopt);

/// The outcome of a reopen after a crash.
struct recovery_check {
  std::string violation;      ///< "" when every check held, else "FAB-INV-RCV-09: ..."
  std::size_t recovered = 0;  ///< k: the head equals the head after k commits
  recovery_report first;      ///< the report of the first reopen
  bool created_again = false; ///< the creation never finished and was redone
};

/// After a crash: reopen with recovery and check that the head is the head after `acked` commits
/// (or `acked + 1` when a commit was in flight) with the oracle's records, that a second reopen
/// reports no loss and the same heads, that a keyed retry of the commit in flight converges on
/// the next head (returning the original receipt when it was durable), and that the next commit
/// of the workload then lands on the oracle's next head.
[[nodiscard]] recovery_check check_recovery(const std::shared_ptr<faulty_vfs> &fs,
                                            const crash_workload &w, const harness_config &cfg,
                                            const crash_run &run);

/// An operation id for intent records: test_id(operation, name).
[[nodiscard]] record_id operation_named(std::string_view name) noexcept;
/// Opens the log store of harness_root() on `fs` directly and commits one batch of `intents`
/// (plus `objects`) through testing::backdoor: intent frames the transaction manager never writes
/// in Volume 04 (WO-052's compensations write them through the store). Returns the commit seq.
[[nodiscard]] result<std::uint64_t> commit_intents(vfs &fs, const log_options &log,
                                                   std::vector<intent_record> intents,
                                                   std::vector<object> objects = {});

/// Files and directories of a vfs_memory under a root.
struct disk_image {
  std::set<std::string> dirs;
  std::map<std::string, std::vector<std::uint8_t>> files;

  friend bool operator==(const disk_image &, const disk_image &) = default;
};
/// The visible state under `root`.
[[nodiscard]] result<disk_image> image_of(vfs_memory &fs, const std::filesystem::path &root);
/// A fresh faulty vfs holding `image`, everything durable, with no primitive counted.
[[nodiscard]] result<std::shared_ptr<faulty_vfs>> restore(const disk_image &image);
/// The same as a plain vfs_memory with nothing synced (no trace and no syncs: faster for sweeps
/// that never crash it).
[[nodiscard]] result<std::shared_ptr<vfs_memory>> restore_memory(const disk_image &image);

/// A workload committed through a fresh durable workspace on a faulty vfs, and the disk image.
struct built_log {
  crash_workload workload;
  std::shared_ptr<faulty_vfs> fs;
  disk_image image;
};
[[nodiscard]] result<built_log> build_log(std::uint64_t seed, std::size_t commits,
                                          const harness_config &cfg, std::size_t records = 1,
                                          std::size_t max_ops = 1);
/// The image path of segment `n` of harness_store().
[[nodiscard]] std::string segment_path(std::uint32_t n);

/// One frame of a segment file.
struct frame_entry {
  std::uint64_t offset = 0;
  std::uint64_t end = 0;
  frame_type type = frame_type::note;
};
/// The frames of a clean segment file (bytes from offset 0, header included).
[[nodiscard]] std::vector<frame_entry> frame_table(byte_span segment);

/// A directory under the ctest working directory (the build tree), removed on both ends.
class scratch_dir {
public:
  explicit scratch_dir(std::string_view name);
  scratch_dir(const scratch_dir &) = delete;
  scratch_dir(scratch_dir &&) = delete;
  scratch_dir &operator=(const scratch_dir &) = delete;
  scratch_dir &operator=(scratch_dir &&) = delete;
  ~scratch_dir();

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return root_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
  std::filesystem::path root_;
  bool ok_ = false;
};

} // namespace sl::fabric::testing
