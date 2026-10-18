// vfs_win32 (WO-053, ADR-0006): the Windows placeholder. Windows durability and locking are
// unverified until a Windows job runs (SL-ARCH-002 seam S14), so every primitive returns
// `unsupported` on every host.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

std::unexpected<error> unsupported(const char *primitive) {
  std::string m("vfs_win32: ");
  m += primitive;
  m += " is unsupported until a Windows job verifies durability and locking (ADR-0006)";
  return make_error(errc::unsupported, std::move(m));
}

} // namespace

result<std::uint64_t> vfs_win32::open_handle(const std::filesystem::path & /*path*/,
                                             const open_options & /*options*/) noexcept {
  return unsupported("open");
}

void vfs_win32::close(std::uint64_t /*handle*/) noexcept {}

result<std::uint64_t> vfs_win32::append(std::uint64_t /*handle*/, byte_span /*data*/) noexcept {
  return unsupported("append");
}

result<void> vfs_win32::pwrite(std::uint64_t /*handle*/, std::uint64_t /*offset*/,
                               byte_span /*data*/) noexcept {
  return unsupported("pwrite");
}

result<std::size_t> vfs_win32::read(std::uint64_t /*handle*/, std::uint64_t /*offset*/,
                                    std::span<std::uint8_t> /*out*/) noexcept {
  return unsupported("read");
}

result<std::uint64_t> vfs_win32::size(std::uint64_t /*handle*/) noexcept {
  return unsupported("size");
}

result<void> vfs_win32::truncate(std::uint64_t /*handle*/, std::uint64_t /*length*/) noexcept {
  return unsupported("truncate");
}

result<void> vfs_win32::fsync(std::uint64_t /*handle*/) noexcept { return unsupported("fsync"); }

result<void> vfs_win32::fdatasync(std::uint64_t /*handle*/) noexcept {
  return unsupported("fdatasync");
}

result<void> vfs_win32::sync_dir(const std::filesystem::path & /*dir*/) noexcept {
  return unsupported("sync_dir");
}

result<void> vfs_win32::rename(const std::filesystem::path & /*from*/,
                               const std::filesystem::path & /*to*/) noexcept {
  return unsupported("rename");
}

result<void> vfs_win32::remove(const std::filesystem::path & /*path*/) noexcept {
  return unsupported("remove");
}

result<void> vfs_win32::make_dir(const std::filesystem::path & /*dir*/) noexcept {
  return unsupported("make_dir");
}

result<std::vector<std::string>> vfs_win32::list(const std::filesystem::path & /*dir*/) noexcept {
  return unsupported("list");
}

result<bool> vfs_win32::exists(const std::filesystem::path & /*path*/) noexcept {
  return unsupported("exists");
}

result<void> vfs_win32::lock_ofd(std::uint64_t /*handle*/) noexcept {
  return unsupported("lock_ofd");
}

result<void> vfs_win32::unlock(std::uint64_t /*handle*/) noexcept { return unsupported("unlock"); }

} // namespace sl::fabric
