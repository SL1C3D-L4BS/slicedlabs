// The vfs seam shared code (WO-053, SL-ARCH-002 ARCH2-R33, ADR-0015): vfs::open, vfs_file and
// read_file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

std::unexpected<error> not_open() {
  return make_error(errc::invalid_argument, "vfs_file: the file is not open");
}

} // namespace

result<vfs_file> vfs::open(const std::filesystem::path &path,
                           const open_options &options) noexcept {
  result<std::uint64_t> handle = open_handle(path, options);
  if (!handle) {
    return std::unexpected(std::move(handle.error()));
  }
  return vfs_file(*this, *handle);
}

vfs_file::vfs_file(vfs_file &&other) noexcept
    : fs_(std::exchange(other.fs_, nullptr)), handle_(std::exchange(other.handle_, 0)) {}

vfs_file &vfs_file::operator=(vfs_file &&other) noexcept {
  if (this != &other) {
    close();
    fs_ = std::exchange(other.fs_, nullptr);
    handle_ = std::exchange(other.handle_, 0);
  }
  return *this;
}

vfs_file::~vfs_file() { close(); }

void vfs_file::close() noexcept {
  if (fs_ != nullptr) {
    fs_->close(handle_);
    fs_ = nullptr;
    handle_ = 0;
  }
}

result<std::uint64_t> vfs_file::append(byte_span data) const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->append(handle_, data);
}

result<void> vfs_file::pwrite(std::uint64_t offset, byte_span data) const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->pwrite(handle_, offset, data);
}

result<std::size_t> vfs_file::read(std::uint64_t offset,
                                   std::span<std::uint8_t> out) const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->read(handle_, offset, out);
}

result<void> vfs_file::read_exact(std::uint64_t offset,
                                  std::span<std::uint8_t> out) const noexcept {
  std::size_t done = 0;
  while (done < out.size()) {
    result<std::size_t> n = read(offset + done, out.subspan(done));
    if (!n) {
      return std::unexpected(std::move(n.error()));
    }
    if (*n == 0) {
      return make_error(errc::decode_truncated, "vfs_file: file ends at offset " +
                                                    std::to_string(offset + done) + ", wanted " +
                                                    std::to_string(out.size() - done) + " more");
    }
    done += *n;
  }
  return {};
}

result<std::uint64_t> vfs_file::size() const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->size(handle_);
}

result<void> vfs_file::truncate(std::uint64_t length) const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->truncate(handle_, length);
}

result<void> vfs_file::fsync() const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->fsync(handle_);
}

result<void> vfs_file::fdatasync() const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->fdatasync(handle_);
}

result<void> vfs_file::lock_ofd() const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->lock_ofd(handle_);
}

result<void> vfs_file::unlock() const noexcept {
  if (fs_ == nullptr) {
    return not_open();
  }
  return fs_->unlock(handle_);
}

result<std::vector<std::uint8_t>> read_file(vfs &fs, const std::filesystem::path &path) noexcept {
  result<vfs_file> file = fs.open(path, open_read);
  if (!file) {
    return std::unexpected(std::move(file.error()));
  }
  const result<std::uint64_t> length = file->size();
  if (!length) {
    return std::unexpected(length.error());
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(*length));
  if (result<void> ok = file->read_exact(0, out); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return out;
}

} // namespace sl::fabric
