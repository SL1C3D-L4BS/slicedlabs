// file_access over sl::fabric::vfs (WO-078, SL-ARCH-003 ARCH3-R038, ADR-0015 pattern). The only
// runtime file that includes sl/fabric/vfs.hpp (runtime-lint R-RT-FABRIC). atomic_write is temp
// file, fsync, rename, fsync of the directory; vfs errors map to not_found or io_error.

#include "sl/core/error.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/platform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

// A vfs error as the runtime reports it: not_found stays, everything else is io_error.
std::unexpected<error> fail(const error &cause, std::string_view primitive,
                            const std::string &path) {
  const errc code = cause.code == errc::not_found ? errc::not_found : errc::io_error;
  return make_error(code,
                    "file_access: " + std::string(primitive) + " " + path + ": " + cause.message);
}

std::unexpected<error> moved_from() {
  return make_error(errc::io_error, "file_access: used after move");
}

std::string parent_of(const std::string &path) {
  std::string parent = std::filesystem::path(path).parent_path().string();
  return parent.empty() ? std::string(".") : parent;
}

} // namespace

file_access::file_access(sl::fabric::vfs &fs, std::string root) noexcept
    : fs_(&fs), root_(std::move(root)) {}

file_access::file_access(file_access &&other) noexcept
    : owned_(std::move(other.owned_)), fs_(std::exchange(other.fs_, nullptr)),
      root_(std::move(other.root_)) {}

file_access &file_access::operator=(file_access &&other) noexcept {
  if (this != &other) {
    owned_ = std::move(other.owned_);
    fs_ = std::exchange(other.fs_, nullptr);
    root_ = std::move(other.root_);
  }
  return *this;
}

file_access::~file_access() = default;

result<file_access> file_access::make_posix(std::string root) noexcept {
  std::unique_ptr<sl::fabric::vfs> fs = std::make_unique<sl::fabric::vfs_posix>();
  std::filesystem::path made;
  for (const std::filesystem::path &part : std::filesystem::path(root)) {
    made /= part;
    if (part.empty() || part == "/" || part == ".") {
      continue;
    }
    auto step = fs->make_dir(made);
    if (!step && step.error().code != errc::already_exists) {
      return fail(step.error(), "make_dir", made.string());
    }
  }
  if (!root.empty()) {
    auto present = fs->exists(root);
    if (!present) {
      return fail(present.error(), "exists", root);
    }
    if (!*present) {
      return make_error(errc::io_error, "file_access::make_posix: " + root + " was not created");
    }
  }
  file_access out(*fs, std::move(root));
  out.owned_ = std::move(fs);
  return out;
}

std::string file_access::resolve(std::string_view path) const {
  if (root_.empty() || path.starts_with('/')) {
    return std::string(path);
  }
  std::string full = root_;
  if (!full.ends_with('/')) {
    full += '/';
  }
  full += path;
  return full;
}

result<std::vector<std::byte>> file_access::read(std::string_view path) const noexcept {
  if (fs_ == nullptr) {
    return moved_from();
  }
  const std::string full = resolve(path);
  auto data = sl::fabric::read_file(*fs_, full);
  if (!data) {
    return fail(data.error(), "read", full);
  }
  std::vector<std::byte> out(data->size());
  std::ranges::transform(*data, out.begin(), [](std::uint8_t b) noexcept { return std::byte{b}; });
  return out;
}

result<void> file_access::atomic_write(std::string_view path,
                                       std::span<const std::byte> bytes) const noexcept {
  if (fs_ == nullptr) {
    return moved_from();
  }
  const std::string full = resolve(path);
  const std::string temp = full + ".tmp";
  std::vector<std::uint8_t> data(bytes.size());
  std::ranges::transform(bytes, data.begin(),
                         [](std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); });
  // best effort: after an injected fault the vfs refuses this too, and the temp file stays
  const auto discard_temp = [this, &temp] noexcept {
    [[maybe_unused]] const auto removed = fs_->remove(temp);
  };
  {
    auto file = fs_->open(temp, sl::fabric::open_create);
    if (!file) {
      return fail(file.error(), "open", temp);
    }
    if (!data.empty()) {
      auto written = file->pwrite(0, data);
      if (!written) {
        file->close();
        discard_temp();
        return fail(written.error(), "pwrite", temp);
      }
    }
    auto synced = file->fsync();
    if (!synced) {
      file->close();
      discard_temp();
      return fail(synced.error(), "fsync", temp);
    }
  }
  auto renamed = fs_->rename(temp, full);
  if (!renamed) {
    discard_temp();
    return fail(renamed.error(), "rename", full);
  }
  const std::string dir = parent_of(full);
  auto dir_synced = fs_->sync_dir(dir);
  if (!dir_synced) {
    return fail(dir_synced.error(), "sync_dir", dir);
  }
  return {};
}

result<void> file_access::remove(std::string_view path) const noexcept {
  if (fs_ == nullptr) {
    return moved_from();
  }
  const std::string full = resolve(path);
  auto removed = fs_->remove(full);
  if (!removed) {
    return fail(removed.error(), "remove", full);
  }
  return {};
}

result<bool> file_access::exists(std::string_view path) const noexcept {
  if (fs_ == nullptr) {
    return moved_from();
  }
  const std::string full = resolve(path);
  auto present = fs_->exists(full);
  if (!present) {
    return fail(present.error(), "exists", full);
  }
  return *present;
}

} // namespace sl::runtime
