// FAB-INV-JRN-05 (SL-ARCH-002 ARCH2-R33): the faulty vfs is exact. drop_unsynced keeps exactly
// the synced bytes and the synced directory entries, keep_unsynced keeps everything,
// tear_last_append keeps exactly k bytes of the last unsynced append, fail_after(n) passes n
// primitives and fails every later one until the crash, flip_bit inverts one bit on read, and the
// trace names every primitive in order. vfs_posix and vfs_memory agree on the primitive
// semantics (posix in a scratch directory under the ctest working directory, inside the build
// tree); vfs_win32 returns unsupported for every primitive.

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/vfs.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::open_append;
using sl::fabric::open_create;
using sl::fabric::open_options;
using sl::fabric::open_read;
using sl::fabric::vfs;
using sl::fabric::vfs_memory;
using sl::fabric::testing::crash_policy;
using sl::fabric::testing::faulty_vfs;

std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

std::string text_of(const std::vector<std::uint8_t> &b) { return {b.begin(), b.end()}; }

// A directory under the ctest working directory (the build tree), removed on both ends.
class scratch_dir {
public:
  explicit scratch_dir(std::string_view name) : root_(root_for(name)) {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_, ec);
    REQUIRE_FALSE(ec);
  }
  scratch_dir(const scratch_dir &) = delete;
  scratch_dir(scratch_dir &&) = delete;
  scratch_dir &operator=(const scratch_dir &) = delete;
  scratch_dir &operator=(scratch_dir &&) = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return root_; }

private:
  static std::filesystem::path root_for(std::string_view name) {
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    REQUIRE_FALSE(ec);
    return cwd / (std::string(name) + ".scratch");
  }

public:
private:
  std::filesystem::path root_;
};

void write(vfs &fs, const std::filesystem::path &p, std::string_view text, bool sync) {
  auto f = fs.open(p, open_options{.write = true, .create = true, .append = true});
  REQUIRE(f.has_value());
  REQUIRE(f->append(bytes_of(text)).has_value());
  if (sync) {
    REQUIRE(f->fsync().has_value());
  }
}

std::string read_all(vfs &fs, const std::filesystem::path &p) {
  const auto b = sl::fabric::read_file(fs, p);
  REQUIRE(b.has_value());
  return text_of(*b);
}

bool present(vfs &fs, const std::filesystem::path &p) {
  const auto e = fs.exists(p);
  REQUIRE(e.has_value());
  return *e;
}

// The primitive semantics every vfs must share; `root` is an existing empty directory.
void vfs_conformance(vfs &fs, const std::filesystem::path &root) {
  const std::filesystem::path a = root / "a";
  CHECK(fs.open(a, open_read).error().code == errc::not_found);
  CHECK(fs.open(a, open_append).error().code == errc::not_found);
  CHECK(fs.open(root / "missing" / "a", open_create).error().code == errc::not_found);
  auto f =
      fs.open(a, open_options{.write = true, .create = true, .exclusive = true, .append = true});
  REQUIRE(f.has_value());
  CHECK(fs.open(a, open_options{.write = true, .create = true, .exclusive = true}).error().code ==
        errc::already_exists);
  CHECK(f->append(bytes_of("hello")).value_or(99) == 0);
  CHECK(f->append(bytes_of(" world")).value_or(99) == 5);
  CHECK(f->size().value_or(0) == 11);
  std::array<std::uint8_t, 8> buf{};
  CHECK(f->read(6, buf).value_or(0) == 5); // short read at the end
  CHECK(text_of({buf.begin(), buf.begin() + 5}) == "world");
  CHECK(f->read(11, buf).value_or(99) == 0);
  CHECK(f->read_exact(8, buf).error().code == errc::decode_truncated);
  CHECK(f->pwrite(0, bytes_of("J")).error().code == errc::invalid_argument); // an append handle
  auto w = fs.open(a, open_options{.write = true});
  REQUIRE(w.has_value());
  REQUIRE(w->pwrite(0, bytes_of("J")).has_value());
  REQUIRE(w->pwrite(13, bytes_of("!")).has_value()); // extends with zeros
  CHECK(f->size().value_or(0) == 14);
  REQUIRE(w->truncate(5).has_value());
  w->close();
  CHECK(f->fsync().has_value());
  CHECK(f->fdatasync().has_value());
  CHECK(read_all(fs, a) == "Jello");

  auto reader = fs.open(a, open_read);
  REQUIRE(reader.has_value());
  CHECK_FALSE(reader->append(bytes_of("x")).has_value()); // not open for writing
  CHECK_FALSE(reader->truncate(0).has_value());

  // OFD locks belong to the open, not the process.
  CHECK(f->lock_ofd().has_value());
  CHECK(f->lock_ofd().has_value()); // re-locking through the same open is fine
  auto second = fs.open(a, open_options{.write = true});
  REQUIRE(second.has_value());
  CHECK(second->lock_ofd().error().code == errc::writer_locked);
  CHECK(f->unlock().has_value());
  CHECK(second->lock_ofd().has_value());
  second->close();
  CHECK(f->lock_ofd().has_value()); // closing released it
  f->close();

  CHECK(fs.make_dir(root / "d").has_value());
  CHECK(fs.make_dir(root / "d").error().code == errc::already_exists);
  CHECK(fs.make_dir(root / "x" / "y").error().code == errc::not_found);
  write(fs, root / "d" / "z", "z", false);
  write(fs, root / "d" / "b", "b", false);
  CHECK(fs.list(root / "d").value_or(std::vector<std::string>{}) ==
        std::vector<std::string>{"b", "z"});
  CHECK(fs.list(root / "nope").error().code == errc::not_found);
  CHECK(fs.sync_dir(root / "d").has_value());
  REQUIRE(fs.rename(root / "d" / "b", root / "d" / "z").has_value()); // replaces
  CHECK(read_all(fs, root / "d" / "z") == "b");
  CHECK_FALSE(present(fs, root / "d" / "b"));
  CHECK(fs.rename(root / "d" / "b", root / "d" / "q").error().code == errc::not_found);
  CHECK_FALSE(fs.remove(root / "d").has_value()); // not empty
  REQUIRE(fs.remove(root / "d" / "z").has_value());
  REQUIRE(fs.remove(root / "d").has_value());
  CHECK_FALSE(present(fs, root / "d"));
  CHECK(fs.remove(root / "d").error().code == errc::not_found);
  CHECK(present(fs, a));
  CHECK(fs.list(root).value_or(std::vector<std::string>{}) == std::vector<std::string>{"a"});
}

} // namespace

TEST_CASE("FAB-INV-JRN-05: vfs_posix and vfs_memory agree on the primitive semantics") {
  SUBCASE("posix") {
    const scratch_dir dir("vfs_faulty_test");
    sl::fabric::vfs_posix fs;
    vfs_conformance(fs, dir.path());
  }
  SUBCASE("memory") {
    vfs_memory fs;
    REQUIRE(fs.make_dir("root").has_value());
    vfs_conformance(fs, "root");
    CHECK(fs.open_handles() == 0);
  }
  SUBCASE("faulty over memory") {
    faulty_vfs fs;
    REQUIRE(fs.make_dir("root").has_value());
    vfs_conformance(fs, "root");
  }
}

TEST_CASE("FAB-INV-JRN-05: drop_unsynced keeps exactly the synced bytes and entries") {
  faulty_vfs fs;
  REQUIRE(fs.make_dir("w").has_value());
  REQUIRE(fs.sync_dir(".").has_value());
  write(fs, "w/f", "abc", true);
  write(fs, "w/f", "def", false);
  REQUIRE(fs.sync_dir("w").has_value());
  write(fs, "w/r", "renamed", true);
  REQUIRE(fs.sync_dir("w").has_value());
  write(fs, "w/new", "unlisted", true);          // synced bytes, entry not synced
  REQUIRE(fs.rename("w/r", "w/r2").has_value()); // rename not synced
  REQUIRE(fs.make_dir("w/sub").has_value());     // directory not synced
  CHECK(read_all(fs, "w/f") == "abcdef");
  REQUIRE(fs.memory() != nullptr);
  CHECK(text_of(fs.memory()->durable_contents("w/f").value()) == "abc");

  REQUIRE(fs.crash(crash_policy::drop_unsynced).has_value());
  CHECK(read_all(fs, "w/f") == "abc");
  CHECK_FALSE(present(fs, "w/new"));
  CHECK(present(fs, "w/r"));
  CHECK_FALSE(present(fs, "w/r2"));
  CHECK_FALSE(present(fs, "w/sub"));
  CHECK(read_all(fs, "w/r") == "renamed");
  CHECK(fs.memory()->open_handles() == 0);

  // A synced rename survives; a directory whose parent entry is not durable takes its files.
  REQUIRE(fs.rename("w/r", "w/r2").has_value());
  REQUIRE(fs.sync_dir("w").has_value());
  REQUIRE(fs.make_dir("w/sub").has_value());
  write(fs, "w/sub/x", "x", true);
  REQUIRE(fs.sync_dir("w/sub").has_value());
  REQUIRE(fs.crash(crash_policy::drop_unsynced).has_value());
  CHECK(present(fs, "w/r2"));
  CHECK_FALSE(present(fs, "w/r"));
  CHECK_FALSE(present(fs, "w/sub"));
}

TEST_CASE("FAB-INV-JRN-05: keep_unsynced keeps every byte and closes every handle") {
  faulty_vfs fs;
  REQUIRE(fs.make_dir("w").has_value());
  auto f = fs.open("w/f", open_options{.write = true, .create = true, .append = true});
  REQUIRE(f.has_value());
  REQUIRE(f->append(bytes_of("unsynced")).has_value());
  REQUIRE(f->lock_ofd().has_value());
  REQUIRE(fs.crash(crash_policy::keep_unsynced).has_value());
  CHECK(read_all(fs, "w/f") == "unsynced");
  auto g = fs.open("w/f", open_options{.write = true});
  REQUIRE(g.has_value());
  CHECK(g->lock_ofd().has_value()); // the crashed process's lock is gone
  f->close();                       // closing a crashed handle is harmless
}

TEST_CASE("FAB-INV-JRN-05: tear_last_append keeps exactly k bytes of the last unsynced append") {
  const std::string head = "header";
  const std::string tail = "0123456789";
  for (std::uint64_t k = 0; k <= tail.size() + 2; ++k) {
    faulty_vfs fs;
    REQUIRE(fs.make_dir("w").has_value());
    REQUIRE(fs.sync_dir(".").has_value());
    write(fs, "w/seg", head, true);
    REQUIRE(fs.sync_dir("w").has_value());
    write(fs, "w/seg", tail, false);
    write(fs, "w/other", "o", false); // not an append to w/seg, but the last append: see below
    REQUIRE(fs.crash(crash_policy::tear_last_append, k).has_value());
    CHECK_FALSE(present(fs, "w/other"));  // entry never synced
    CHECK(read_all(fs, "w/seg") == head); // the torn append was to w/other

    faulty_vfs fs2;
    REQUIRE(fs2.make_dir("w").has_value());
    REQUIRE(fs2.sync_dir(".").has_value());
    write(fs2, "w/seg", head, true);
    REQUIRE(fs2.sync_dir("w").has_value());
    write(fs2, "w/seg", tail, false);
    REQUIRE(fs2.crash(crash_policy::tear_last_append, k).has_value());
    CHECK(read_all(fs2, "w/seg") == head + tail.substr(0, static_cast<std::size_t>(k)));
  }
  // A synced last append is not torn: tear_last_append then equals drop_unsynced.
  faulty_vfs fs;
  REQUIRE(fs.make_dir("w").has_value());
  REQUIRE(fs.sync_dir(".").has_value());
  write(fs, "w/seg", head, true);
  REQUIRE(fs.sync_dir("w").has_value());
  REQUIRE(fs.crash(crash_policy::tear_last_append, 2).has_value());
  CHECK(read_all(fs, "w/seg") == head);
}

TEST_CASE("FAB-INV-JRN-05: fail_after(n) passes n primitives, then fails until the crash") {
  for (std::uint64_t n = 0; n <= 5; ++n) {
    faulty_vfs fs;
    REQUIRE(fs.make_dir("w").has_value());
    fs.clear_trace();
    const std::uint64_t base = fs.primitives();
    fs.fail_after(n);
    std::vector<bool> ok;
    ok.push_back(fs.exists("w").has_value());              // 0
    ok.push_back(fs.make_dir("w/a").has_value());          // 1
    ok.push_back(fs.list("w").has_value());                // 2
    ok.push_back(fs.sync_dir("w").has_value());            // 3
    ok.push_back(fs.open("w/f", open_create).has_value()); // 4
    ok.push_back(fs.exists("w/a").has_value());            // 5
    for (std::uint64_t i = 0; i < ok.size(); ++i) {
      CHECK(ok[i] == (i < n));
    }
    CHECK(fs.primitives() == base + 6);
    CHECK(fs.injected() == 6 - n);
    CHECK(fs.dead() == (n < 6));
    std::vector<std::string> trace;
    for (const std::string &line : fs.trace()) {
      if (!line.starts_with("close ")) { // close is traced, never counted
        trace.push_back(line);
      }
    }
    REQUIRE(trace.size() == 6);
    for (std::uint64_t i = 0; i < trace.size(); ++i) {
      CHECK(trace[i].ends_with(" !fail") == (i >= n));
    }
    const auto failed = fs.exists("w");
    if (n < 6) {
      REQUIRE_FALSE(failed.has_value());
      CHECK(failed.error().code == errc::store_io);
      CHECK(failed.error().message.find("injected failure") != std::string::npos);
    }
    REQUIRE(fs.crash(crash_policy::keep_unsynced).has_value());
    CHECK_FALSE(fs.dead());
    CHECK(fs.exists("w").has_value());
  }
}

TEST_CASE("FAB-INV-JRN-05: flip_bit inverts one bit on every read of that file") {
  faulty_vfs fs;
  REQUIRE(fs.make_dir("w").has_value());
  write(fs, "w/f", "AAAA", true);
  write(fs, "w/g", "AAAA", true);
  fs.flip_bit("w/f", 2, 0);
  fs.flip_bit("w/f", 3, 5);
  CHECK(read_all(fs, "w/f") == "AA@a");
  CHECK(read_all(fs, "w/g") == "AAAA");
  CHECK(text_of(fs.memory()->contents("w/f").value()) == "AAAA"); // the stored bytes are intact
  fs.clear_flips();
  CHECK(read_all(fs, "w/f") == "AAAA");
}

TEST_CASE("FAB-INV-JRN-05: the trace names every primitive in order") {
  faulty_vfs fs;
  REQUIRE(fs.make_dir("w").has_value());
  {
    auto f = fs.open("w/f", open_options{.write = true, .create = true, .append = true});
    REQUIRE(f.has_value());
    REQUIRE(f->append(bytes_of("abc")).has_value());
    REQUIRE(f->fsync().has_value());
  }
  REQUIRE(fs.rename("w/f", "w/g").has_value());
  REQUIRE(fs.sync_dir("w").has_value());
  CHECK(fs.trace() == std::vector<std::string>{"make_dir w", "open f wca", "append f +3", "fsync f",
                                               "close f", "rename f g", "sync_dir w"});
}

TEST_CASE("FAB-INV-JRN-05: a crash over a vfs that is not vfs_memory keeps unsynced data only") {
  const scratch_dir dir("vfs_faulty_test_posix");
  faulty_vfs fs(std::make_shared<sl::fabric::vfs_posix>());
  CHECK(fs.memory() == nullptr);
  write(fs, dir.path() / "f", "data", false);
  CHECK(fs.crash(crash_policy::drop_unsynced).error().code == errc::unsupported);
  CHECK(fs.crash(crash_policy::tear_last_append, 1).error().code == errc::unsupported);
  REQUIRE(fs.crash(crash_policy::keep_unsynced).has_value());
  CHECK(read_all(fs, dir.path() / "f") == "data");
}

TEST_CASE("FAB-INV-JRN-05: vfs_win32 returns unsupported for every primitive") {
  sl::fabric::vfs_win32 fs;
  std::array<std::uint8_t, 4> buf{};
  const std::vector<errc> codes = {
      fs.open("f", open_read).error().code,
      fs.append(1, buf).error().code,
      fs.pwrite(1, 0, buf).error().code,
      fs.read(1, 0, buf).error().code,
      fs.size(1).error().code,
      fs.truncate(1, 0).error().code,
      fs.fsync(1).error().code,
      fs.fdatasync(1).error().code,
      fs.sync_dir("d").error().code,
      fs.rename("a", "b").error().code,
      fs.remove("a").error().code,
      fs.make_dir("d").error().code,
      fs.list("d").error().code,
      fs.exists("a").error().code,
      fs.lock_ofd(1).error().code,
      fs.unlock(1).error().code,
  };
  for (const errc c : codes) {
    CHECK(c == errc::unsupported);
  }
  fs.close(1); // a no-op
}
