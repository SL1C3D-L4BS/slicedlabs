// RT-INV-PLAT-06 (SL-ARCH-003 ARCH3-R038, ADR-0015 pattern): file_access over the faulty vfs
// returns io_error for an injected fault and atomic_write leaves the old or the new content after
// a crash at each primitive. The crash sweep counts the primitives of one clean atomic_write, then
// for each k fails primitive k (and every later one), crashes the vfs (drop_unsynced and
// keep_unsynced) and reads the file back.

#include "sl/core/error.hpp"
#include "sl/runtime/platform.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;
namespace ft = sl::fabric::testing;

std::vector<std::byte> old_content() { return std::vector<std::byte>(3, std::byte{1}); }
std::vector<std::byte> new_content() { return std::vector<std::byte>(5, std::byte{2}); }

} // namespace

TEST_CASE("RT-INV-PLAT-06: an injected vfs fault is io_error, a missing file not_found") {
  rtt::scripted_platform sp;
  rt::file_access &files = sp.files();
  REQUIRE(files.atomic_write("a.bin", old_content()).has_value());

  sp.vfs().fail_after(0);
  auto read = files.read("a.bin");
  REQUIRE_FALSE(read.has_value());
  CHECK(read.error().code == sl::errc::io_error);
  auto write = files.atomic_write("a.bin", new_content());
  REQUIRE_FALSE(write.has_value());
  CHECK(write.error().code == sl::errc::io_error);
  auto present = files.exists("a.bin");
  REQUIRE_FALSE(present.has_value());
  CHECK(present.error().code == sl::errc::io_error);
  auto removed = files.remove("a.bin");
  REQUIRE_FALSE(removed.has_value());
  CHECK(removed.error().code == sl::errc::io_error);

  sp.vfs().disarm();
  REQUIRE(sp.vfs().crash(ft::crash_policy::keep_unsynced).has_value());
  auto back = files.read("a.bin");
  REQUIRE(back.has_value());
  CHECK(*back == old_content());
  auto missing = files.read("missing.bin");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == sl::errc::not_found);
  auto gone = files.remove("missing.bin");
  REQUIRE_FALSE(gone.has_value());
  CHECK(gone.error().code == sl::errc::not_found);
  CHECK(files.exists("a.bin").value_or(false));
  CHECK_FALSE(files.exists("missing.bin").value_or(true));
}

TEST_CASE("RT-INV-PLAT-06: atomic_write leaves the old or the new content after a crash") {
  // the primitives of one clean atomic_write over an existing file
  std::uint64_t primitives = 0;
  {
    ft::faulty_vfs fs;
    const rt::file_access files(fs, "/");
    REQUIRE(files.atomic_write("s.bin", old_content()).has_value());
    const std::uint64_t before = fs.primitives();
    REQUIRE(files.atomic_write("s.bin", new_content()).has_value());
    primitives = fs.primitives() - before;
    const std::vector<std::string> trace = fs.trace();
    REQUIRE(trace.size() >= 4);
  }
  REQUIRE(primitives >= 4); // open, pwrite, fsync, rename, sync_dir

  for (const ft::crash_policy policy :
       {ft::crash_policy::drop_unsynced, ft::crash_policy::keep_unsynced}) {
    std::uint64_t saw_old = 0;
    std::uint64_t saw_new = 0;
    for (std::uint64_t k = 0; k <= primitives; ++k) {
      CAPTURE(k);
      ft::faulty_vfs fs;
      const rt::file_access files(fs, "/");
      REQUIRE(files.atomic_write("s.bin", old_content()).has_value());
      fs.fail_after(k);
      auto written = files.atomic_write("s.bin", new_content());
      if (k < primitives) {
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().code == sl::errc::io_error);
      } else {
        CHECK(written.has_value());
      }
      REQUIRE(fs.crash(policy).has_value());
      auto back = files.read("s.bin");
      REQUIRE(back.has_value());
      const bool is_old = *back == old_content();
      const bool is_new = *back == new_content();
      CHECK((is_old || is_new));
      saw_old += is_old ? 1U : 0U;
      saw_new += is_new ? 1U : 0U;
      if (k == primitives) {
        CHECK(is_new);
      }
    }
    CHECK(saw_old > 0);
    CHECK(saw_new > 0);
  }
}

TEST_CASE("RT-INV-PLAT-06: file_access resolves relative paths against its root and is move-only") {
  ft::faulty_vfs fs;
  REQUIRE(fs.make_dir("/data").has_value());
  rt::file_access files(fs, "/data");
  REQUIRE(files.atomic_write("x.bin", old_content()).has_value());
  CHECK(files.exists("/data/x.bin").value_or(false));
  CHECK_FALSE(files.exists("/x.bin").value_or(true));
  rt::file_access moved = std::move(files);
  CHECK(moved.root() == "/data");
  auto back = moved.read("x.bin");
  REQUIRE(back.has_value());
  CHECK(*back == old_content());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the moved-from state
  auto stale = files.read("x.bin");
  REQUIRE_FALSE(stale.has_value());
  CHECK(stale.error().code == sl::errc::io_error);
}
