#pragma once

// The storage primitives under the durable log (WO-053, SL-ARCH-002 ARCH2-R28, R33 and R34,
// ADR-0015, ADR-0006).
//
// A vfs is the only way the log backend touches files: open, append, pwrite, read, size,
// truncate, fsync, fdatasync, sync_dir, rename, remove, make_dir, list, exists, lock_ofd and
// unlock. Every primitive is noexcept and returns sl::result; nothing throws (ADR-0009).
//
//   vfs_posix   open/pread/pwrite/fsync/rename with errno mapped to error codes; appends go through
//               an O_APPEND descriptor; lock_ofd is an OFD write lock (F_OFD_SETLK). On a host
//               without POSIX every primitive returns `unsupported`.
//   vfs_memory  an exact durability model for tests: file bytes and directory entries each have
//               a visible state and a durable one. fsync/fdatasync make a file's bytes durable,
//               sync_dir makes the entries of one directory durable, crash() throws away
//               everything that was not made durable.
//   vfs_win32   every primitive returns `unsupported` until a Windows job runs (ADR-0006).
//
// testing/faulty_vfs wraps any vfs with fault injection (fail after N primitives, crash policies,
// bit flips on read) and records the primitive trace.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"

namespace sl::fabric {

/// How vfs::open opens a file. A read-only open ignores create, exclusive, truncate and append.
struct open_options {
  bool write = false;     ///< read-write instead of read-only
  bool create = false;    ///< create the file when it is absent
  bool exclusive = false; ///< with create: `already_exists` when the file is present
  bool truncate = false;  ///< with write: empty the file
  bool append = false;    ///< with write: every write goes to the end (O_APPEND)
};

/// A read-only open.
inline constexpr open_options open_read{};
/// A read-write open of an existing file whose writes go to the end.
inline constexpr open_options open_append{.write = true, .append = true};
/// A read-write open that creates or empties the file.
inline constexpr open_options open_create{.write = true, .create = true, .truncate = true};

class vfs;

/// An open file of a vfs: owns its handle and closes it when destroyed. Move-only; the vfs must
/// outlive it. Every member forwards to the vfs primitive of the same name.
class vfs_file {
public:
  vfs_file() noexcept = default;
  vfs_file(vfs &fs, std::uint64_t handle) noexcept : fs_(&fs), handle_(handle) {}
  vfs_file(const vfs_file &) = delete;
  vfs_file &operator=(const vfs_file &) = delete;
  vfs_file(vfs_file &&other) noexcept;
  vfs_file &operator=(vfs_file &&other) noexcept;
  ~vfs_file();

  [[nodiscard]] bool is_open() const noexcept { return fs_ != nullptr; }
  [[nodiscard]] std::uint64_t handle() const noexcept { return handle_; }

  /// Appends `data` at the end of the file; returns the offset its first byte landed at.
  [[nodiscard]] result<std::uint64_t> append(byte_span data) const noexcept;
  [[nodiscard]] result<void> pwrite(std::uint64_t offset, byte_span data) const noexcept;
  /// Reads up to out.size() bytes at `offset`; fewer only at the end of the file.
  [[nodiscard]] result<std::size_t> read(std::uint64_t offset,
                                         std::span<std::uint8_t> out) const noexcept;
  /// Reads exactly out.size() bytes at `offset`; `decode_truncated` if the file ends first.
  [[nodiscard]] result<void> read_exact(std::uint64_t offset,
                                        std::span<std::uint8_t> out) const noexcept;
  [[nodiscard]] result<std::uint64_t> size() const noexcept;
  [[nodiscard]] result<void> truncate(std::uint64_t length) const noexcept;
  [[nodiscard]] result<void> fsync() const noexcept;
  [[nodiscard]] result<void> fdatasync() const noexcept;
  [[nodiscard]] result<void> lock_ofd() const noexcept;
  [[nodiscard]] result<void> unlock() const noexcept;
  /// Closes the handle now (the destructor does it otherwise).
  void close() noexcept;

private:
  vfs *fs_ = nullptr;
  std::uint64_t handle_ = 0;
};

/// The file-system seam of the log backend. Every member is thread-safe; handles are opaque
/// numbers that are never 0. `not_found` names a missing file or directory, `already_exists`
/// an existing one, `writer_locked` a lock held through another open, `store_io` any other
/// failure; messages name the primitive and the path.
class vfs {
public:
  vfs() = default;
  vfs(const vfs &) = delete;
  vfs(vfs &&) = delete;
  vfs &operator=(const vfs &) = delete;
  vfs &operator=(vfs &&) = delete;
  virtual ~vfs() = default;

  /// Opens `path`; the returned file closes the handle when destroyed.
  [[nodiscard]] result<vfs_file> open(const std::filesystem::path &path,
                                      const open_options &options) noexcept;

  [[nodiscard]] virtual result<std::uint64_t> open_handle(const std::filesystem::path &path,
                                                          const open_options &options) noexcept = 0;
  virtual void close(std::uint64_t handle) noexcept = 0;
  [[nodiscard]] virtual result<std::uint64_t> append(std::uint64_t handle,
                                                     byte_span data) noexcept = 0;
  /// Writes at `offset`, extending the file with zeros if needed; `invalid_argument` on a handle
  /// opened for append (where POSIX would append whatever the offset).
  [[nodiscard]] virtual result<void> pwrite(std::uint64_t handle, std::uint64_t offset,
                                            byte_span data) noexcept = 0;
  [[nodiscard]] virtual result<std::size_t> read(std::uint64_t handle, std::uint64_t offset,
                                                 std::span<std::uint8_t> out) noexcept = 0;
  [[nodiscard]] virtual result<std::uint64_t> size(std::uint64_t handle) noexcept = 0;
  [[nodiscard]] virtual result<void> truncate(std::uint64_t handle,
                                              std::uint64_t length) noexcept = 0;
  [[nodiscard]] virtual result<void> fsync(std::uint64_t handle) noexcept = 0;
  [[nodiscard]] virtual result<void> fdatasync(std::uint64_t handle) noexcept = 0;
  /// Makes the entries of directory `dir` (creations, renames, removals) durable.
  [[nodiscard]] virtual result<void> sync_dir(const std::filesystem::path &dir) noexcept = 0;
  /// Atomically replaces `to` with `from` (files only).
  [[nodiscard]] virtual result<void> rename(const std::filesystem::path &from,
                                            const std::filesystem::path &to) noexcept = 0;
  /// Removes a file or an empty directory.
  [[nodiscard]] virtual result<void> remove(const std::filesystem::path &path) noexcept = 0;
  /// Creates one directory; its parent must exist.
  [[nodiscard]] virtual result<void> make_dir(const std::filesystem::path &dir) noexcept = 0;
  /// The entry names of a directory, sorted bytewise, without "." and "..".
  [[nodiscard]] virtual result<std::vector<std::string>>
  list(const std::filesystem::path &dir) noexcept = 0;
  [[nodiscard]] virtual result<bool> exists(const std::filesystem::path &path) noexcept = 0;
  /// An exclusive advisory lock on the whole file, owned by this open (not by the process);
  /// `writer_locked` when another open holds it.
  [[nodiscard]] virtual result<void> lock_ofd(std::uint64_t handle) noexcept = 0;
  [[nodiscard]] virtual result<void> unlock(std::uint64_t handle) noexcept = 0;
};

/// The POSIX file system (open/pread/pwrite/fsync/rename, O_APPEND, F_OFD_SETLK).
class vfs_posix final : public vfs {
public:
  vfs_posix();
  vfs_posix(const vfs_posix &) = delete;
  vfs_posix(vfs_posix &&) = delete;
  vfs_posix &operator=(const vfs_posix &) = delete;
  vfs_posix &operator=(vfs_posix &&) = delete;
  ~vfs_posix() override;

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
  struct state;
  std::unique_ptr<state> state_;
};

/// An in-memory file system with an exact durability model (see the file comment). Paths are
/// compared after lexical normalisation; the root ("" and "/") always exists.
class vfs_memory final : public vfs {
public:
  vfs_memory();
  vfs_memory(const vfs_memory &) = delete;
  vfs_memory(vfs_memory &&) = delete;
  vfs_memory &operator=(const vfs_memory &) = delete;
  vfs_memory &operator=(vfs_memory &&) = delete;
  ~vfs_memory() override;

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

  /// A power loss: every handle is closed and every lock released; the directory entries
  /// become their durable state and every file's bytes its last synced bytes.
  void crash() noexcept;
  /// The visible bytes of a file (`not_found` if absent).
  [[nodiscard]] result<std::vector<std::uint8_t>>
  contents(const std::filesystem::path &path) const noexcept;
  /// The bytes of a file that would survive crash() (`not_found` if its entry would not).
  [[nodiscard]] result<std::vector<std::uint8_t>>
  durable_contents(const std::filesystem::path &path) const noexcept;
  /// The number of open handles.
  [[nodiscard]] std::size_t open_handles() const noexcept;

private:
  struct state;
  std::unique_ptr<state> state_;
};

/// The Windows placeholder (ADR-0006): every primitive returns `unsupported`.
class vfs_win32 final : public vfs {
public:
  vfs_win32() = default;
  vfs_win32(const vfs_win32 &) = delete;
  vfs_win32(vfs_win32 &&) = delete;
  vfs_win32 &operator=(const vfs_win32 &) = delete;
  vfs_win32 &operator=(vfs_win32 &&) = delete;
  ~vfs_win32() override = default;

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
};

/// Reads a whole file through `fs`.
[[nodiscard]] result<std::vector<std::uint8_t>>
read_file(vfs &fs, const std::filesystem::path &path) noexcept;

} // namespace sl::fabric
