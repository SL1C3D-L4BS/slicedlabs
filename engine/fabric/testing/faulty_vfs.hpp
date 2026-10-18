#pragma once

// Fault injection over any vfs (WO-053, SL-ARCH-002 ARCH2-R33, FAB-INV-JRN-05; used by WO-055's
// crash harness). Include as "../../../engine/fabric/testing/faulty_vfs.hpp" from tests.
//
//   fail_after(n)           primitives 0..n-1 (counted from now) pass; primitive n fails with
//                           store_io and so does every later one until crash() (the process died)
//   crash(policy, k)        a power loss or process crash, then the vfs works again:
//                             keep_unsynced     every written byte survives (process crash only)
//                             drop_unsynced     only synced bytes and synced directory entries
//                                               survive (vfs_memory::crash)
//                             tear_last_append  as drop_unsynced, except that the file of the last
//                                               append not yet synced keeps its bytes up to k bytes
//                                               into that append
//                           drop_unsynced and tear_last_append need a vfs_memory underneath
//   flip_bit(path, off, b)  every later read of `path` sees bit `b` of byte `off` inverted
//   trace()                 every primitive called, in order: "<primitive> <file name> [detail]",
//                           with " !fail" when it was failed by injection
//
// close() is traced but neither counted nor failed.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric::testing {

enum class crash_policy : std::uint8_t { keep_unsynced, drop_unsynced, tear_last_append };

class faulty_vfs final : public vfs {
public:
  /// Over a fresh vfs_memory.
  faulty_vfs();
  /// Over `inner`.
  explicit faulty_vfs(std::shared_ptr<vfs> inner);
  faulty_vfs(const faulty_vfs &) = delete;
  faulty_vfs(faulty_vfs &&) = delete;
  faulty_vfs &operator=(const faulty_vfs &) = delete;
  faulty_vfs &operator=(faulty_vfs &&) = delete;
  ~faulty_vfs() override;

  /// The wrapped vfs, and the same as a vfs_memory (nullptr if it is not one).
  [[nodiscard]] vfs &inner() const noexcept { return *inner_; }
  [[nodiscard]] vfs_memory *memory() const noexcept;

  void fail_after(std::uint64_t n);
  void disarm();
  [[nodiscard]] bool dead() const;
  /// `unsupported` for drop_unsynced or tear_last_append over a vfs that is not a vfs_memory.
  [[nodiscard]] result<void> crash(crash_policy policy, std::uint64_t tear_at = 0);
  void flip_bit(const std::filesystem::path &path, std::uint64_t offset, unsigned bit);
  void clear_flips();

  /// The number of counted primitives so far (failed ones included).
  [[nodiscard]] std::uint64_t primitives() const;
  /// The number of primitives failed by injection.
  [[nodiscard]] std::uint64_t injected() const;
  [[nodiscard]] std::vector<std::string> trace() const;
  void clear_trace();

  [[nodiscard]] result<std::uint64_t> open_handle(const std::filesystem::path &path,
                                                  const open_options &options) noexcept override;
  void close(std::uint64_t handle) noexcept override;
  [[nodiscard]] result<std::uint64_t> append(std::uint64_t handle,
                                             byte_span data) noexcept override;
  [[nodiscard]] result<void> pwrite(std::uint64_t handle, std::uint64_t offset,
                                    byte_span data) noexcept override;
  [[nodiscard]] result<std::size_t> read(std::uint64_t handle, std::uint64_t offset,
                                         std::span<std::uint8_t> out) noexcept override;
  [[nodiscard]] result<std::uint64_t> size(std::uint64_t handle) noexcept override;
  [[nodiscard]] result<void> truncate(std::uint64_t handle, std::uint64_t length) noexcept override;
  [[nodiscard]] result<void> fsync(std::uint64_t handle) noexcept override;
  [[nodiscard]] result<void> fdatasync(std::uint64_t handle) noexcept override;
  [[nodiscard]] result<void> sync_dir(const std::filesystem::path &dir) noexcept override;
  [[nodiscard]] result<void> rename(const std::filesystem::path &from,
                                    const std::filesystem::path &to) noexcept override;
  [[nodiscard]] result<void> remove(const std::filesystem::path &path) noexcept override;
  [[nodiscard]] result<void> make_dir(const std::filesystem::path &dir) noexcept override;
  [[nodiscard]] result<std::vector<std::string>>
  list(const std::filesystem::path &dir) noexcept override;
  [[nodiscard]] result<bool> exists(const std::filesystem::path &path) noexcept override;
  [[nodiscard]] result<void> lock_ofd(std::uint64_t handle) noexcept override;
  [[nodiscard]] result<void> unlock(std::uint64_t handle) noexcept override;

private:
  struct last_append {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool synced = false;
  };
  struct flip {
    std::uint64_t offset = 0;
    std::uint8_t mask = 0;
  };

  // Counts and traces one primitive; returns the injected failure, if any.
  [[nodiscard]] std::optional<error> gate(std::string_view primitive, std::string_view detail);
  [[nodiscard]] std::string path_of(std::uint64_t handle) const;
  void note_sync(std::uint64_t handle);

  std::shared_ptr<vfs> inner_;
  mutable std::mutex mutex_;
  std::optional<std::uint64_t> fail_at_;
  bool dead_ = false;
  std::uint64_t primitives_ = 0;
  std::uint64_t injected_ = 0;
  std::vector<std::string> trace_;
  std::map<std::uint64_t, std::string> handles_; // inner handle -> normalised path
  std::optional<last_append> last_append_;
  std::map<std::string, std::vector<flip>> flips_;
};

} // namespace sl::fabric::testing
