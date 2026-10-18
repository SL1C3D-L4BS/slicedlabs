// vfs_memory (WO-053, SL-ARCH-002 ARCH2-R33, ADR-0015): an in-memory file system with an exact
// durability model. Each file (inode) holds its visible bytes and its synced bytes; each name
// lives in a visible directory table and a durable one. fsync/fdatasync copy a file's visible
// bytes to its synced bytes, sync_dir copies one directory's visible entries to the durable
// table, and crash() replaces the visible state by the durable one.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

struct inode {
  std::vector<std::uint8_t> data;
  std::vector<std::uint8_t> synced;
  std::uint64_t lock_owner = 0; // the handle holding the lock, 0 if none
};

struct entry {
  bool dir = false;
  std::shared_ptr<inode> node; // null for a directory
};

struct open_file {
  std::shared_ptr<inode> node;
  bool write = false;
  bool append = false;
  std::string path;
};

using table = std::map<std::string, entry, std::less<>>;

std::string key_of(const std::filesystem::path &p) {
  std::string k = p.lexically_normal().generic_string();
  while (k.size() > 1 && k.back() == '/') {
    k.pop_back();
  }
  if (k == ".") {
    k.clear();
  }
  return k;
}

std::string parent_of(std::string_view key) {
  const std::size_t slash = key.rfind('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  if (slash == 0) {
    return "/";
  }
  return std::string(key.substr(0, slash));
}

std::string name_of(std::string_view key) {
  const std::size_t slash = key.rfind('/');
  return std::string(slash == std::string_view::npos ? key : key.substr(slash + 1));
}

bool is_root(std::string_view key) noexcept { return key.empty() || key == "/"; }

bool is_dir(const table &t, std::string_view key) {
  if (is_root(key)) {
    return true;
  }
  const auto it = t.find(key);
  return it != t.end() && it->second.dir;
}

// Whether every ancestor of `key` is a directory of `t`.
bool rooted(const table &t, std::string_view key) {
  for (std::string p = parent_of(key); !is_root(p); p = parent_of(p)) {
    if (!is_dir(t, p)) {
      return false;
    }
  }
  return true;
}

std::unexpected<error> mem_error(errc code, std::string_view what, std::string_view path,
                                 std::string_view why) {
  std::string m("vfs_memory: ");
  m += what;
  m += " '";
  m += path;
  m += "': ";
  m += why;
  return make_error(code, std::move(m));
}

std::unexpected<error> bad_handle(std::uint64_t handle) {
  return make_error(errc::invalid_argument,
                    "vfs_memory: handle " + std::to_string(handle) + " is not open");
}

} // namespace

struct vfs_memory::state {
  mutable std::mutex mutex;
  table visible;
  table durable;
  std::map<std::uint64_t, open_file> handles;
  std::uint64_t next = 1;

  open_file *find(std::uint64_t handle) {
    const auto it = handles.find(handle);
    return it == handles.end() ? nullptr : &it->second;
  }
};

vfs_memory::vfs_memory() : state_(std::make_unique<state>()) {}

vfs_memory::~vfs_memory() = default;

result<std::uint64_t> vfs_memory::open_handle(const std::filesystem::path &path,
                                              const open_options &options) noexcept {
  const std::string key = key_of(path);
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  std::shared_ptr<inode> node;
  if (const auto it = s.visible.find(key); it != s.visible.end()) {
    if (it->second.dir) {
      return mem_error(errc::store_io, "open", key, "is a directory");
    }
    if (options.write && options.create && options.exclusive) {
      return mem_error(errc::already_exists, "open", key, "exists");
    }
    node = it->second.node;
    if (options.write && options.truncate) {
      node->data.clear();
    }
  } else if (is_root(key)) {
    return mem_error(errc::store_io, "open", key, "is a directory");
  } else {
    if (!options.write || !options.create) {
      return mem_error(errc::not_found, "open", key, "no such file");
    }
    if (!is_dir(s.visible, parent_of(key)) || !rooted(s.visible, key)) {
      return mem_error(errc::not_found, "open", key, "no such directory");
    }
    node = std::make_shared<inode>();
    s.visible.insert_or_assign(key, entry{.dir = false, .node = node});
  }
  const std::uint64_t handle = s.next++;
  s.handles.emplace(handle, open_file{.node = std::move(node),
                                      .write = options.write,
                                      .append = options.write && options.append,
                                      .path = key});
  return handle;
}

void vfs_memory::close(std::uint64_t handle) noexcept {
  const std::scoped_lock lock(state_->mutex);
  const auto it = state_->handles.find(handle);
  if (it == state_->handles.end()) {
    return;
  }
  if (it->second.node->lock_owner == handle) {
    it->second.node->lock_owner = 0;
  }
  state_->handles.erase(it);
}

result<std::uint64_t> vfs_memory::append(std::uint64_t handle, byte_span data) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  if (!f->write) {
    return mem_error(errc::store_io, "append", f->path, "not open for writing");
  }
  const std::uint64_t at = f->node->data.size();
  f->node->data.insert(f->node->data.end(), data.begin(), data.end());
  return at;
}

result<void> vfs_memory::pwrite(std::uint64_t handle, std::uint64_t offset,
                                byte_span data) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  if (!f->write) {
    return mem_error(errc::store_io, "pwrite", f->path, "not open for writing");
  }
  if (f->append) {
    return mem_error(errc::invalid_argument, "pwrite", f->path, "the handle is opened for append");
  }
  std::vector<std::uint8_t> &bytes = f->node->data;
  const auto end = static_cast<std::size_t>(offset) + data.size();
  if (bytes.size() < end) {
    bytes.resize(end, 0);
  }
  std::ranges::copy(data, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  return {};
}

result<std::size_t> vfs_memory::read(std::uint64_t handle, std::uint64_t offset,
                                     std::span<std::uint8_t> out) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  const std::vector<std::uint8_t> &bytes = f->node->data;
  if (offset >= bytes.size()) {
    return std::size_t{0};
  }
  const std::size_t n = std::min(out.size(), bytes.size() - static_cast<std::size_t>(offset));
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
  return n;
}

result<std::uint64_t> vfs_memory::size(std::uint64_t handle) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  return static_cast<std::uint64_t>(f->node->data.size());
}

result<void> vfs_memory::truncate(std::uint64_t handle, std::uint64_t length) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  if (!f->write) {
    return mem_error(errc::store_io, "truncate", f->path, "not open for writing");
  }
  f->node->data.resize(static_cast<std::size_t>(length), 0);
  return {};
}

result<void> vfs_memory::fsync(std::uint64_t handle) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  f->node->synced = f->node->data;
  return {};
}

result<void> vfs_memory::fdatasync(std::uint64_t handle) noexcept { return fsync(handle); }

result<void> vfs_memory::sync_dir(const std::filesystem::path &dir) noexcept {
  const std::string key = key_of(dir);
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  if (!is_dir(s.visible, key)) {
    return mem_error(errc::not_found, "sync_dir", key, "no such directory");
  }
  std::erase_if(s.durable, [&key](const auto &e) { return parent_of(e.first) == key; });
  for (const auto &[name, e] : s.visible) {
    if (parent_of(name) == key) {
      s.durable.insert_or_assign(name, e);
    }
  }
  return {};
}

result<void> vfs_memory::rename(const std::filesystem::path &from,
                                const std::filesystem::path &to) noexcept {
  const std::string src = key_of(from);
  const std::string dst = key_of(to);
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  const auto it = s.visible.find(src);
  if (it == s.visible.end()) {
    return mem_error(errc::not_found, "rename", src, "no such file");
  }
  if (it->second.dir) {
    return mem_error(errc::store_io, "rename", src, "directories cannot be renamed");
  }
  if (!is_dir(s.visible, parent_of(dst))) {
    return mem_error(errc::not_found, "rename", dst, "no such directory");
  }
  if (is_dir(s.visible, dst)) {
    return mem_error(errc::store_io, "rename", dst, "is a directory");
  }
  if (src == dst) {
    return {};
  }
  entry moved = it->second;
  s.visible.erase(it);
  s.visible.insert_or_assign(dst, std::move(moved));
  return {};
}

result<void> vfs_memory::remove(const std::filesystem::path &path) noexcept {
  const std::string key = key_of(path);
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  const auto it = s.visible.find(key);
  if (it == s.visible.end()) {
    return mem_error(errc::not_found, "remove", key, "no such file or directory");
  }
  if (it->second.dir) {
    for (const auto &e : s.visible) {
      if (parent_of(e.first) == key) {
        return mem_error(errc::store_io, "remove", key, "directory not empty");
      }
    }
  }
  s.visible.erase(it);
  return {};
}

result<void> vfs_memory::make_dir(const std::filesystem::path &dir) noexcept {
  const std::string key = key_of(dir);
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  if (is_root(key) || s.visible.contains(key)) {
    return mem_error(errc::already_exists, "make_dir", key, "exists");
  }
  if (!is_dir(s.visible, parent_of(key)) || !rooted(s.visible, key)) {
    return mem_error(errc::not_found, "make_dir", key, "no such parent directory");
  }
  s.visible.insert_or_assign(key, entry{.dir = true, .node = nullptr});
  return {};
}

result<std::vector<std::string>> vfs_memory::list(const std::filesystem::path &dir) noexcept {
  const std::string key = key_of(dir);
  const std::scoped_lock lock(state_->mutex);
  const state &s = *state_;
  if (!is_dir(s.visible, key)) {
    return mem_error(errc::not_found, "list", key, "no such directory");
  }
  std::vector<std::string> names;
  for (const auto &e : s.visible) {
    if (parent_of(e.first) == key && !is_root(e.first)) {
      names.push_back(name_of(e.first));
    }
  }
  std::ranges::sort(names);
  return names;
}

result<bool> vfs_memory::exists(const std::filesystem::path &path) noexcept {
  const std::string key = key_of(path);
  const std::scoped_lock lock(state_->mutex);
  return is_root(key) || state_->visible.contains(key);
}

result<void> vfs_memory::lock_ofd(std::uint64_t handle) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  if (f->node->lock_owner != 0 && f->node->lock_owner != handle) {
    return mem_error(errc::writer_locked, "lock_ofd", f->path,
                     "locked by handle " + std::to_string(f->node->lock_owner));
  }
  f->node->lock_owner = handle;
  return {};
}

result<void> vfs_memory::unlock(std::uint64_t handle) noexcept {
  const std::scoped_lock lock(state_->mutex);
  open_file *f = state_->find(handle);
  if (f == nullptr) {
    return bad_handle(handle);
  }
  if (f->node->lock_owner == handle) {
    f->node->lock_owner = 0;
  }
  return {};
}

void vfs_memory::crash() noexcept {
  const std::scoped_lock lock(state_->mutex);
  state &s = *state_;
  s.handles.clear();
  table survivors;
  for (const auto &[name, e] : s.durable) {
    if (rooted(s.durable, name)) {
      survivors.insert_or_assign(name, e);
    }
  }
  for (auto &[name, e] : survivors) {
    if (e.node) {
      e.node->data = e.node->synced;
      e.node->lock_owner = 0;
    }
  }
  s.durable = survivors;
  s.visible = std::move(survivors);
}

result<std::vector<std::uint8_t>>
vfs_memory::contents(const std::filesystem::path &path) const noexcept {
  const std::string key = key_of(path);
  const std::scoped_lock lock(state_->mutex);
  const auto it = state_->visible.find(key);
  if (it == state_->visible.end() || it->second.dir) {
    return mem_error(errc::not_found, "contents", key, "no such file");
  }
  return it->second.node->data;
}

result<std::vector<std::uint8_t>>
vfs_memory::durable_contents(const std::filesystem::path &path) const noexcept {
  const std::string key = key_of(path);
  const std::scoped_lock lock(state_->mutex);
  const auto it = state_->durable.find(key);
  if (it == state_->durable.end() || it->second.dir || !rooted(state_->durable, key)) {
    return mem_error(errc::not_found, "durable_contents", key, "no durable file");
  }
  return it->second.node->synced;
}

std::size_t vfs_memory::open_handles() const noexcept {
  const std::scoped_lock lock(state_->mutex);
  return state_->handles.size();
}

} // namespace sl::fabric
