// faulty_vfs (WO-053, FAB-INV-JRN-05): fault injection and a primitive trace over any vfs.

#include "faulty_vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric::testing {

namespace {

std::string key_of(const std::filesystem::path &p) {
  std::string k = p.lexically_normal().generic_string();
  while (k.size() > 1 && k.back() == '/') {
    k.pop_back();
  }
  return k;
}

std::string name_of_key(std::string_view key) {
  const std::size_t slash = key.rfind('/');
  return std::string(slash == std::string_view::npos ? key : key.substr(slash + 1));
}

std::string file_name(const std::filesystem::path &p) { return name_of_key(key_of(p)); }

std::string_view policy_name(crash_policy p) noexcept {
  switch (p) {
  case crash_policy::keep_unsynced:
    return "keep_unsynced";
  case crash_policy::drop_unsynced:
    return "drop_unsynced";
  case crash_policy::tear_last_append:
    return "tear_last_append";
  }
  return "unknown";
}

} // namespace

faulty_vfs::faulty_vfs() : inner_(std::make_shared<vfs_memory>()) {}

faulty_vfs::faulty_vfs(std::shared_ptr<vfs> inner) : inner_(std::move(inner)) {}

faulty_vfs::~faulty_vfs() = default;

vfs_memory *faulty_vfs::memory() const noexcept { return dynamic_cast<vfs_memory *>(inner_.get()); }

void faulty_vfs::fail_after(std::uint64_t n) {
  const std::scoped_lock lock(mutex_);
  fail_at_ = primitives_ + n;
}

void faulty_vfs::disarm() {
  const std::scoped_lock lock(mutex_);
  fail_at_.reset();
}

bool faulty_vfs::dead() const {
  const std::scoped_lock lock(mutex_);
  return dead_;
}

result<void> faulty_vfs::crash(crash_policy policy, std::uint64_t tear_at) {
  std::map<std::uint64_t, std::string> handles;
  std::optional<last_append> torn;
  {
    const std::scoped_lock lock(mutex_);
    handles.swap(handles_);
    torn = last_append_;
    last_append_.reset();
    dead_ = false;
    fail_at_.reset();
    trace_.push_back(std::string("crash ") + std::string(policy_name(policy)));
  }
  vfs_memory *mem = memory();
  if (policy != crash_policy::keep_unsynced && mem == nullptr) {
    return make_error(errc::unsupported, "faulty_vfs: drop_unsynced and tear_last_append need a "
                                         "vfs_memory underneath");
  }
  if (policy == crash_policy::tear_last_append && torn && !torn->synced) {
    // The torn file keeps its bytes up to `tear_at` bytes into the last append.
    result<vfs_file> f = inner_->open(torn->path, open_options{.write = true});
    if (f) {
      const std::uint64_t keep = torn->offset + std::min(tear_at, torn->length);
      if (result<void> ok = f->truncate(keep); !ok) {
        return ok;
      }
      if (result<void> ok = f->fsync(); !ok) {
        return ok;
      }
    }
  }
  if (mem != nullptr && policy != crash_policy::keep_unsynced) {
    mem->crash();
  } else {
    for (const auto &entry : handles) {
      inner_->close(entry.first);
    }
  }
  return {};
}

void faulty_vfs::flip_bit(const std::filesystem::path &path, std::uint64_t offset, unsigned bit) {
  const std::scoped_lock lock(mutex_);
  flips_[key_of(path)].push_back(
      flip{.offset = offset, .mask = static_cast<std::uint8_t>(1U << (bit & 7U))});
}

void faulty_vfs::clear_flips() {
  const std::scoped_lock lock(mutex_);
  flips_.clear();
}

std::uint64_t faulty_vfs::primitives() const {
  const std::scoped_lock lock(mutex_);
  return primitives_;
}

std::uint64_t faulty_vfs::injected() const {
  const std::scoped_lock lock(mutex_);
  return injected_;
}

std::vector<std::string> faulty_vfs::trace() const {
  const std::scoped_lock lock(mutex_);
  return trace_;
}

void faulty_vfs::clear_trace() {
  const std::scoped_lock lock(mutex_);
  trace_.clear();
}

std::optional<error> faulty_vfs::gate(std::string_view primitive, std::string_view detail) {
  const std::scoped_lock lock(mutex_);
  const std::uint64_t index = primitives_++;
  std::string line(primitive);
  if (!detail.empty()) {
    line += ' ';
    line += detail;
  }
  if (!dead_ && fail_at_ && index >= *fail_at_) {
    dead_ = true;
  }
  if (dead_) {
    ++injected_;
    trace_.push_back(line + " !fail");
    return error{.code = errc::store_io,
                 .message = "faulty_vfs: injected failure of primitive " + std::to_string(index) +
                            " (" + line + ")",
                 .where = std::source_location::current()};
  }
  trace_.push_back(std::move(line));
  return std::nullopt;
}

std::string faulty_vfs::path_of(std::uint64_t handle) const {
  const std::scoped_lock lock(mutex_);
  const auto it = handles_.find(handle);
  return it == handles_.end() ? std::string("?") : it->second;
}

void faulty_vfs::note_sync(std::uint64_t handle) {
  const std::scoped_lock lock(mutex_);
  const auto it = handles_.find(handle);
  if (last_append_ && it != handles_.end() && it->second == last_append_->path) {
    last_append_->synced = true;
  }
}

result<std::uint64_t> faulty_vfs::open_handle(const std::filesystem::path &path,
                                              const open_options &options) noexcept {
  std::string mode = options.write ? "w" : "r";
  if (options.create) {
    mode += 'c';
  }
  if (options.truncate) {
    mode += 't';
  }
  if (options.append) {
    mode += 'a';
  }
  if (const std::optional<error> e = gate("open", file_name(path) + " " + mode)) {
    return std::unexpected(*e);
  }
  const result<std::uint64_t> h = inner_->open_handle(path, options);
  if (h) {
    const std::scoped_lock lock(mutex_);
    handles_.insert_or_assign(*h, key_of(path));
  }
  return h;
}

void faulty_vfs::close(std::uint64_t handle) noexcept {
  {
    const std::scoped_lock lock(mutex_);
    const auto it = handles_.find(handle);
    if (it == handles_.end()) {
      return; // closed by a crash already
    }
    trace_.push_back("close " + name_of_key(it->second));
    handles_.erase(it);
  }
  inner_->close(handle);
}

result<std::uint64_t> faulty_vfs::append(std::uint64_t handle, byte_span data) noexcept {
  const std::string path = path_of(handle);
  if (const std::optional<error> e =
          gate("append", name_of_key(path) + " +" + std::to_string(data.size()))) {
    return std::unexpected(*e);
  }
  const result<std::uint64_t> at = inner_->append(handle, data);
  if (at) {
    const std::scoped_lock lock(mutex_);
    last_append_ = last_append{.path = path, .offset = *at, .length = data.size(), .synced = false};
  }
  return at;
}

result<void> faulty_vfs::pwrite(std::uint64_t handle, std::uint64_t offset,
                                byte_span data) noexcept {
  if (const std::optional<error> e =
          gate("pwrite", name_of_key(path_of(handle)) + " @" + std::to_string(offset) + " +" +
                             std::to_string(data.size()))) {
    return std::unexpected(*e);
  }
  return inner_->pwrite(handle, offset, data);
}

result<std::size_t> faulty_vfs::read(std::uint64_t handle, std::uint64_t offset,
                                     std::span<std::uint8_t> out) noexcept {
  const std::string path = path_of(handle);
  if (const std::optional<error> e =
          gate("read", name_of_key(path) + " @" + std::to_string(offset))) {
    return std::unexpected(*e);
  }
  const result<std::size_t> n = inner_->read(handle, offset, out);
  if (n) {
    const std::scoped_lock lock(mutex_);
    const auto it = flips_.find(path);
    if (it != flips_.end()) {
      for (const flip &f : it->second) {
        if (f.offset >= offset && f.offset < offset + *n) {
          out[static_cast<std::size_t>(f.offset - offset)] ^= f.mask;
        }
      }
    }
  }
  return n;
}

result<std::uint64_t> faulty_vfs::size(std::uint64_t handle) noexcept {
  if (const std::optional<error> e = gate("size", name_of_key(path_of(handle)))) {
    return std::unexpected(*e);
  }
  return inner_->size(handle);
}

result<void> faulty_vfs::truncate(std::uint64_t handle, std::uint64_t length) noexcept {
  if (const std::optional<error> e =
          gate("truncate", name_of_key(path_of(handle)) + " " + std::to_string(length))) {
    return std::unexpected(*e);
  }
  return inner_->truncate(handle, length);
}

result<void> faulty_vfs::fsync(std::uint64_t handle) noexcept {
  if (const std::optional<error> e = gate("fsync", name_of_key(path_of(handle)))) {
    return std::unexpected(*e);
  }
  result<void> ok = inner_->fsync(handle);
  if (ok) {
    note_sync(handle);
  }
  return ok;
}

result<void> faulty_vfs::fdatasync(std::uint64_t handle) noexcept {
  if (const std::optional<error> e = gate("fdatasync", name_of_key(path_of(handle)))) {
    return std::unexpected(*e);
  }
  result<void> ok = inner_->fdatasync(handle);
  if (ok) {
    note_sync(handle);
  }
  return ok;
}

result<void> faulty_vfs::sync_dir(const std::filesystem::path &dir) noexcept {
  if (const std::optional<error> e = gate("sync_dir", file_name(dir))) {
    return std::unexpected(*e);
  }
  return inner_->sync_dir(dir);
}

result<void> faulty_vfs::rename(const std::filesystem::path &from,
                                const std::filesystem::path &to) noexcept {
  if (const std::optional<error> e = gate("rename", file_name(from) + " " + file_name(to))) {
    return std::unexpected(*e);
  }
  return inner_->rename(from, to);
}

result<void> faulty_vfs::remove(const std::filesystem::path &path) noexcept {
  if (const std::optional<error> e = gate("remove", file_name(path))) {
    return std::unexpected(*e);
  }
  return inner_->remove(path);
}

result<void> faulty_vfs::make_dir(const std::filesystem::path &dir) noexcept {
  if (const std::optional<error> e = gate("make_dir", file_name(dir))) {
    return std::unexpected(*e);
  }
  return inner_->make_dir(dir);
}

result<std::vector<std::string>> faulty_vfs::list(const std::filesystem::path &dir) noexcept {
  if (const std::optional<error> e = gate("list", file_name(dir))) {
    return std::unexpected(*e);
  }
  return inner_->list(dir);
}

result<bool> faulty_vfs::exists(const std::filesystem::path &path) noexcept {
  if (const std::optional<error> e = gate("exists", file_name(path))) {
    return std::unexpected(*e);
  }
  return inner_->exists(path);
}

result<void> faulty_vfs::lock_ofd(std::uint64_t handle) noexcept {
  if (const std::optional<error> e = gate("lock_ofd", name_of_key(path_of(handle)))) {
    return std::unexpected(*e);
  }
  return inner_->lock_ofd(handle);
}

result<void> faulty_vfs::unlock(std::uint64_t handle) noexcept {
  if (const std::optional<error> e = gate("unlock", name_of_key(path_of(handle)))) {
    return std::unexpected(*e);
  }
  return inner_->unlock(handle);
}

} // namespace sl::fabric::testing
