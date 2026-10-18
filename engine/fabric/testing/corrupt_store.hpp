#pragma once

// Planted corruption for the corruption tests (WO-059, SL-ARCH-002 `corruption` C1-C12).
//
// make_sample_store() writes a small durable workspace over the test schemas (test_schemas.hpp)
// into a vfs_memory: commit 1 creates the Mesh "m0" and the Entity "e1", every later commit k
// creates the Entity "e<k>" and renames "e1". The plants then damage it the way the corruption
// table describes, each returning where it struck:
//   plant_truncate         the last segment loses its last `bytes` bytes (C1)
//   plant_flip_object      one byte flipped in the middle of the payload of the object frame in
//                          the middle of segment 1 (C2; the reached object is then C5)
//   plant_delete_commit    the frames of a middle commit (its objects and its commit frame) cut
//                          out of segment 1 (C3)
//   plant_remove_loose     a loose blob removed (C5)
//   plant_flip_loose       one byte of a loose blob flipped (C4)
//   plant_newer_format     FORMAT rewritten (C8)
//   plant_stale_lock       a holder text left in the lock file with no writer (C11)
//   plant_broken_checkpoint one byte of the newest checkpoint flipped (C12; make_checkpoint first)
// rewrite_head() commits a new head of "main" through testing::backdoor whose records trie is the
// old one with `changes` applied: the way record-level damage (C6, C9, C10) is planted without a
// transaction.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric::testing {

struct sample_config {
  /// Commits on "main" (at least 1).
  std::size_t commits = 6;
  /// Objects larger than this go to loose/ (the default keeps them all in the log).
  std::uint64_t loose_above = std::uint64_t{1} << 20U;
  std::uint64_t rotate_at = std::uint64_t{1} << 20U;
  /// Built-in validators left out (workspace_options::omit_builtin).
  std::vector<std::string> omit_builtin;
};

struct sample_store {
  std::shared_ptr<vfs_memory> fs;
  /// The workspace directory and its store directory.
  std::filesystem::path root;
  std::filesystem::path dir;
  /// heads[k]: the head of "main" after commit k + 1.
  std::vector<digest> heads;
  /// The commit sequence numbers, parallel to heads.
  std::vector<std::uint64_t> seqs;
};

/// The workspace options of a sample (scene_options plus the log settings and omissions) over
/// `fs`.
[[nodiscard]] result<workspace_options> sample_options(std::shared_ptr<vfs> fs,
                                                       const sample_config &cfg);

/// Writes the sample (file comment) into a new vfs_memory under "/cor" and closes it.
[[nodiscard]] result<sample_store> make_sample_store(const sample_config &cfg = {});

/// workspace::open(write) of the sample.
[[nodiscard]] result<std::unique_ptr<workspace>> open_sample(const sample_store &s,
                                                             const sample_config &cfg = {});

/// Where a plant struck.
struct planted {
  /// Relative to the store directory.
  std::string file;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::optional<digest> oid;
  /// plant_delete_commit: the seq of the commit cut out.
  std::uint64_t seq = 0;
};

[[nodiscard]] result<planted> plant_truncate(vfs &fs, const std::filesystem::path &dir,
                                             std::uint64_t bytes);
[[nodiscard]] result<planted> plant_flip_object(vfs &fs, const std::filesystem::path &dir);
[[nodiscard]] result<planted> plant_delete_commit(vfs &fs, const std::filesystem::path &dir);
[[nodiscard]] result<planted> plant_remove_loose(vfs &fs, const std::filesystem::path &dir);
[[nodiscard]] result<planted> plant_flip_loose(vfs &fs, const std::filesystem::path &dir);
[[nodiscard]] result<planted> plant_newer_format(vfs &fs, const std::filesystem::path &dir,
                                                 std::string_view text = "slfabric 2.0\n");
[[nodiscard]] result<planted> plant_stale_lock(vfs &fs, const std::filesystem::path &dir,
                                               std::string_view holder = "pid 4242 host gone");
[[nodiscard]] result<planted> plant_broken_checkpoint(vfs &fs, const std::filesystem::path &dir);

/// Makes recovery write a checkpoint: appends a torn frame prefix to the last segment, then opens
/// the sample for write (the repair writes the checkpoint) and closes it.
[[nodiscard]] result<void> make_checkpoint(const sample_store &s, const sample_config &cfg = {});

/// Commits through testing::backdoor a new head of `ref` on top of the current one: its records
/// trie is the current one with `changes` applied (membership, schema set and tx unchanged), and
/// `extra` objects are stored with it. Returns the new revision.
[[nodiscard]] result<digest> rewrite_head(store_backend &store, std::vector<tree_change> changes,
                                          std::vector<object> extra, std::string_view ref = "main");

/// rewrite_head over the closed sample's log store (recover_log repair, then the backdoor).
[[nodiscard]] result<digest> rewrite_sample(const sample_store &s, std::vector<tree_change> changes,
                                            std::vector<object> extra,
                                            const sample_config &cfg = {});

/// rewrite_sample putting `o` (stored as is) as the record entry of `id`.
[[nodiscard]] result<digest> plant_record(const sample_store &s, const record_id &id, object o,
                                          const sample_config &cfg = {});

/// The decoded record object of `id` at `revision`.
[[nodiscard]] result<record_object> record_at(const store_backend &store, const digest &revision,
                                              const record_id &id);

/// Writes `bytes` over `path` (created or truncated).
[[nodiscard]] result<void> write_bytes(vfs &fs, const std::filesystem::path &path, byte_span bytes);

} // namespace sl::fabric::testing
