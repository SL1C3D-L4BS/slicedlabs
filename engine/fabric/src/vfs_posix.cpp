// vfs_posix (WO-053, SL-ARCH-002 ARCH2-R33 and R34, ADR-0015): open/pread/pwrite/fsync/rename with
// errno mapped to error codes, O_APPEND appends and OFD advisory locks. A host without POSIX gets
// the same class with every primitive returning `unsupported` (ADR-0006).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define SL_FABRIC_VFS_POSIX 1
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#else
#define SL_FABRIC_VFS_POSIX 0
#endif

namespace sl::fabric {

#if SL_FABRIC_VFS_POSIX

struct vfs_posix::state {
  std::shared_mutex mutex;
  struct open_fd {
    int fd = -1;
    bool append = false;
  };
  std::map<std::uint64_t, open_fd> fds; // handle -> descriptor
  std::uint64_t next = 1;
};

namespace {

std::string describe_errno(std::string_view what, const std::filesystem::path &path, int err) {
  std::string m("vfs_posix: ");
  m += what;
  if (!path.empty()) {
    m += " '";
    m += path.string();
    m += '\'';
  }
  m += ": ";
  m += std::generic_category().message(err);
  return m;
}

errc code_for(int err) noexcept {
  switch (err) {
  case ENOENT:
  case ENOTDIR:
    return errc::not_found;
  case EEXIST:
    return errc::already_exists;
  default:
    return errc::store_io;
  }
}

std::unexpected<error> posix_error(std::string_view what, const std::filesystem::path &path,
                                   int err) {
  return make_error(code_for(err), describe_errno(what, path, err));
}

std::unexpected<error> bad_handle(std::uint64_t handle) {
  return make_error(errc::invalid_argument,
                    "vfs_posix: handle " + std::to_string(handle) + " is not open");
}

// Whole-file write lock or unlock through an open file description.
int ofd_lock(int fd, short type) noexcept {
  struct flock fl{};
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
#ifdef F_OFD_SETLK
  return ::fcntl(fd, F_OFD_SETLK, &fl); // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX API
#else
  return ::fcntl(fd, F_SETLK, &fl); // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX API
#endif
}

} // namespace

vfs_posix::vfs_posix() : state_(std::make_unique<state>()) {}

vfs_posix::~vfs_posix() {
  const std::unique_lock lock(state_->mutex);
  for (const auto &entry : state_->fds) {
    ::close(entry.second.fd);
  }
  state_->fds.clear();
}

result<std::uint64_t> vfs_posix::open_handle(const std::filesystem::path &path,
                                             const open_options &options) noexcept {
  // NOLINTBEGIN(bugprone-signed-bitwise): POSIX open flags are int masks
  int flags = O_CLOEXEC;
  if (options.write) {
    flags |= O_RDWR;
    if (options.create) {
      flags |= O_CREAT;
      if (options.exclusive) {
        flags |= O_EXCL;
      }
    }
    if (options.truncate) {
      flags |= O_TRUNC;
    }
    if (options.append) {
      flags |= O_APPEND;
    }
  } else {
    flags |= O_RDONLY;
  }
  // NOLINTEND(bugprone-signed-bitwise)
  int fd = -1;
  do {
    fd = ::open(path.c_str(), flags, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX API
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return posix_error("open", path, errno);
  }
  const std::unique_lock lock(state_->mutex);
  const std::uint64_t handle = state_->next++;
  state_->fds.emplace(handle, state::open_fd{.fd = fd, .append = options.write && options.append});
  return handle;
}

void vfs_posix::close(std::uint64_t handle) noexcept {
  int fd = -1;
  {
    const std::unique_lock lock(state_->mutex);
    const auto it = state_->fds.find(handle);
    if (it == state_->fds.end()) {
      return;
    }
    fd = it->second.fd;
    state_->fds.erase(it);
  }
  ::close(fd);
}

namespace {

// The descriptor of a handle, or -1; `append` tells whether it was opened for appending.
template <class Map>
int fd_of(std::shared_mutex &mutex, const Map &fds, std::uint64_t handle, bool *append = nullptr) {
  const std::shared_lock lock(mutex);
  const auto it = fds.find(handle);
  if (it == fds.end()) {
    return -1;
  }
  if (append != nullptr) {
    *append = it->second.append;
  }
  return it->second.fd;
}

result<void> write_all(int fd, byte_span data, std::string_view what) noexcept {
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(fd, data.subspan(done).data(), data.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error(what, {}, errno);
    }
    done += static_cast<std::size_t>(n);
  }
  return {};
}

} // namespace

result<std::uint64_t> vfs_posix::append(std::uint64_t handle, byte_span data) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  const off_t end = ::lseek(fd, 0, SEEK_END);
  if (end < 0) {
    return posix_error("append (lseek)", {}, errno);
  }
  if (result<void> ok = write_all(fd, data, "append"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return static_cast<std::uint64_t>(end);
}

result<void> vfs_posix::pwrite(std::uint64_t handle, std::uint64_t offset,
                               byte_span data) noexcept {
  bool append = false;
  const int fd = fd_of(state_->mutex, state_->fds, handle, &append);
  if (fd < 0) {
    return bad_handle(handle);
  }
  if (append) {
    // Linux would append regardless of the offset; the vfs refuses instead.
    return make_error(errc::invalid_argument, "vfs_posix: pwrite on a handle opened for append");
  }
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::pwrite(fd, data.subspan(done).data(), data.size() - done,
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error("pwrite", {}, errno);
    }
    done += static_cast<std::size_t>(n);
  }
  return {};
}

result<std::size_t> vfs_posix::read(std::uint64_t handle, std::uint64_t offset,
                                    std::span<std::uint8_t> out) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t n =
        ::pread(fd, out.subspan(done).data(), out.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error("pread", {}, errno);
    }
    if (n == 0) {
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  return done;
}

result<std::uint64_t> vfs_posix::size(std::uint64_t handle) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return posix_error("fstat", {}, errno);
  }
  return static_cast<std::uint64_t>(st.st_size);
}

result<void> vfs_posix::truncate(std::uint64_t handle, std::uint64_t length) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    return posix_error("ftruncate", {}, errno);
  }
  return {};
}

result<void> vfs_posix::fsync(std::uint64_t handle) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  if (::fsync(fd) != 0) {
    return posix_error("fsync", {}, errno);
  }
  return {};
}

result<void> vfs_posix::fdatasync(std::uint64_t handle) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
#ifdef __APPLE__
  const int rc = ::fsync(fd);
#else
  const int rc = ::fdatasync(fd);
#endif
  if (rc != 0) {
    return posix_error("fdatasync", {}, errno);
  }
  return {};
}

result<void> vfs_posix::sync_dir(const std::filesystem::path &dir) noexcept {
  int fd = -1;
  do {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX API
    fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return posix_error("sync_dir (open)", dir, errno);
  }
  const int rc = ::fsync(fd);
  const int err = errno;
  ::close(fd);
  if (rc != 0) {
    return posix_error("sync_dir (fsync)", dir, err);
  }
  return {};
}

result<void> vfs_posix::rename(const std::filesystem::path &from,
                               const std::filesystem::path &to) noexcept {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return posix_error("rename to '" + to.string() + "' from", from, errno);
  }
  return {};
}

result<void> vfs_posix::remove(const std::filesystem::path &path) noexcept {
  if (::unlink(path.c_str()) == 0) {
    return {};
  }
  if (errno == EISDIR || errno == EPERM) {
    if (::rmdir(path.c_str()) == 0) {
      return {};
    }
  }
  return posix_error("remove", path, errno);
}

result<void> vfs_posix::make_dir(const std::filesystem::path &dir) noexcept {
  if (::mkdir(dir.c_str(), 0755) != 0) {
    return posix_error("make_dir", dir, errno);
  }
  return {};
}

result<std::vector<std::string>> vfs_posix::list(const std::filesystem::path &dir) noexcept {
  DIR *d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return posix_error("list", dir, errno);
  }
  std::vector<std::string> names;
  errno = 0;
  for (const dirent *e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    const std::string_view name(static_cast<const char *>(e->d_name));
    if (name != "." && name != "..") {
      names.emplace_back(name);
    }
  }
  const int err = errno;
  ::closedir(d);
  if (err != 0) {
    return posix_error("list (readdir)", dir, err);
  }
  std::ranges::sort(names);
  return names;
}

result<bool> vfs_posix::exists(const std::filesystem::path &path) noexcept {
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) {
    return true;
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  return posix_error("exists", path, errno);
}

result<void> vfs_posix::lock_ofd(std::uint64_t handle) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  if (ofd_lock(fd, F_WRLCK) != 0) {
    const int err = errno;
    if (err == EAGAIN || err == EACCES) {
      return make_error(errc::writer_locked, describe_errno("lock_ofd", {}, err));
    }
    return posix_error("lock_ofd", {}, err);
  }
  return {};
}

result<void> vfs_posix::unlock(std::uint64_t handle) noexcept {
  const int fd = fd_of(state_->mutex, state_->fds, handle);
  if (fd < 0) {
    return bad_handle(handle);
  }
  if (ofd_lock(fd, F_UNLCK) != 0) {
    return posix_error("unlock", {}, errno);
  }
  return {};
}

#else // no POSIX: the interface exists, every primitive is unsupported (ADR-0006)

struct vfs_posix::state {};

namespace {
std::unexpected<error> no_posix() {
  return make_error(errc::unsupported, "vfs_posix: this host has no POSIX file API");
}
} // namespace

vfs_posix::vfs_posix() : state_(std::make_unique<state>()) {}
vfs_posix::~vfs_posix() = default;
result<std::uint64_t> vfs_posix::open_handle(const std::filesystem::path & /*path*/,
                                             const open_options & /*options*/) noexcept {
  return no_posix();
}
void vfs_posix::close(std::uint64_t /*handle*/) noexcept {}
result<std::uint64_t> vfs_posix::append(std::uint64_t /*handle*/, byte_span /*data*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::pwrite(std::uint64_t /*handle*/, std::uint64_t /*offset*/,
                               byte_span /*data*/) noexcept {
  return no_posix();
}
result<std::size_t> vfs_posix::read(std::uint64_t /*handle*/, std::uint64_t /*offset*/,
                                    std::span<std::uint8_t> /*out*/) noexcept {
  return no_posix();
}
result<std::uint64_t> vfs_posix::size(std::uint64_t /*handle*/) noexcept { return no_posix(); }
result<void> vfs_posix::truncate(std::uint64_t /*handle*/, std::uint64_t /*length*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::fsync(std::uint64_t /*handle*/) noexcept { return no_posix(); }
result<void> vfs_posix::fdatasync(std::uint64_t /*handle*/) noexcept { return no_posix(); }
result<void> vfs_posix::sync_dir(const std::filesystem::path & /*dir*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::rename(const std::filesystem::path & /*from*/,
                               const std::filesystem::path & /*to*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::remove(const std::filesystem::path & /*path*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::make_dir(const std::filesystem::path & /*dir*/) noexcept {
  return no_posix();
}
result<std::vector<std::string>> vfs_posix::list(const std::filesystem::path & /*dir*/) noexcept {
  return no_posix();
}
result<bool> vfs_posix::exists(const std::filesystem::path & /*path*/) noexcept {
  return no_posix();
}
result<void> vfs_posix::lock_ofd(std::uint64_t /*handle*/) noexcept { return no_posix(); }
result<void> vfs_posix::unlock(std::uint64_t /*handle*/) noexcept { return no_posix(); }

#endif

} // namespace sl::fabric
