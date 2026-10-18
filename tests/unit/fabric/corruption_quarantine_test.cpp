// FAB-INV-COR-02 (SL-ARCH-002 ARCH2-R41): quarantine never deletes; quarantined bytes equal the
// damaged bytes. quarantine() copies a whole file or a byte range into `.slfabric/quarantine/`,
// keeps the source, reuses a name holding the same bytes, never overwrites a name holding other
// bytes (it takes "-2"), moves only whole files and refuses bad names. A lenient open of a store
// with mid-log damage and of one with an undecodable record copies the damaged segment and the
// damaged object into quarantine/ byte for byte, and every file the store held before the open is
// still there with the same bytes.

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/corrupt_store.hpp"
#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

using bytes_t = std::vector<std::uint8_t>;

std::filesystem::path store_dir() { return "/q/.slfabric"; }

// A whole-file copy of `source` (relative to the store) named `name`.
quarantine_item item_of(const std::filesystem::path &source, const std::string &name) {
  quarantine_item item;
  item.store_dir = store_dir();
  item.source = source;
  item.name = name;
  return item;
}

// The same, for the byte range [offset, offset + length).
quarantine_item ranged(const std::filesystem::path &source, const std::string &name,
                       std::uint64_t offset, std::uint64_t length) {
  quarantine_item item = item_of(source, name);
  item.offset = offset;
  item.length = length;
  return item;
}

bytes_t read(vfs &fs, const std::filesystem::path &p) { return must(read_file(fs, p)); }

std::shared_ptr<vfs_memory> store_with(const bytes_t &seg) {
  auto fs = std::make_shared<vfs_memory>();
  REQUIRE(fs->make_dir("/q").has_value());
  REQUIRE(fs->make_dir(store_dir()).has_value());
  REQUIRE(fs->make_dir(store_dir() / "log").has_value());
  REQUIRE(tg::write_bytes(*fs, store_dir() / "log/seg-00000001.slg", seg).has_value());
  return fs;
}

// Every file of `before` is in `after` with the same bytes; new files are under quarantine/.
void nothing_lost(const tg::disk_image &before, const tg::disk_image &after) {
  for (const auto &[path, bytes] : before.files) {
    CAPTURE(path);
    const auto it = after.files.find(path);
    REQUIRE(it != after.files.end());
    CHECK(it->second == bytes);
  }
  for (const auto &[path, bytes] : after.files) {
    if (!before.files.contains(path)) {
      CAPTURE(path);
      CHECK(path.contains("/quarantine/"));
    }
  }
}

} // namespace

TEST_CASE("FAB-INV-COR-02: a copy keeps the source and holds exactly its bytes") {
  const bytes_t seg = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  const auto fs = store_with(seg);
  const std::string q = must(quarantine(*fs, item_of("log/seg-00000001.slg", "3-seg")));
  CHECK(q == "quarantine/3-seg");
  CHECK(read(*fs, store_dir() / q) == seg);
  CHECK(read(*fs, store_dir() / "log/seg-00000001.slg") == seg);

  // A range: exactly the damaged bytes.
  const std::string r = must(quarantine(*fs, ranged("log/seg-00000001.slg", "3-tail", 6, 4)));
  CHECK(read(*fs, store_dir() / r) == bytes_t{7, 8, 9, 10});
  CHECK(read(*fs, store_dir() / "log/seg-00000001.slg") == seg);
}

TEST_CASE("FAB-INV-COR-02: a taken name is never overwritten and the same bytes are reused") {
  const auto fs = store_with({1, 2, 3});
  const quarantine_item item = item_of("log/seg-00000001.slg", "x");
  CHECK(must(quarantine(*fs, item)) == "quarantine/x");
  CHECK(must(quarantine(*fs, item)) == "quarantine/x"); // a rerun copies nothing twice
  REQUIRE(tg::write_bytes(*fs, store_dir() / "log/seg-00000001.slg", bytes_t{9, 9}).has_value());
  CHECK(must(quarantine(*fs, item)) == "quarantine/x-2");
  CHECK(read(*fs, store_dir() / "quarantine/x") == bytes_t{1, 2, 3});
  CHECK(read(*fs, store_dir() / "quarantine/x-2") == bytes_t{9, 9});
  CHECK(must(fs->list(store_dir() / "quarantine")).size() == 2);
}

TEST_CASE("FAB-INV-COR-02: a move keeps the bytes; bad items are refused and touch nothing") {
  const bytes_t seg = {4, 5, 6};
  const auto fs = store_with(seg);
  REQUIRE(tg::write_bytes(*fs, store_dir() / "log/extra", seg).has_value());
  quarantine_item move = item_of("log/extra", "moved");
  move.method = quarantine_method::move;
  const std::string m = must(quarantine(*fs, move));
  CHECK(read(*fs, store_dir() / m) == seg);
  CHECK_FALSE(must(fs->exists(store_dir() / "log/extra")));

  const tg::disk_image before = must(tg::image_of(*fs, "/q"));
  for (const std::string name : {"", "a/b"}) {
    const auto r = quarantine(*fs, item_of("log/seg-00000001.slg", name));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::invalid_argument);
  }
  quarantine_item ranged_item = ranged("log/seg-00000001.slg", "r", 0, 1);
  ranged_item.method = quarantine_method::move;
  const auto ranged_move = quarantine(*fs, ranged_item);
  REQUIRE_FALSE(ranged_move.has_value());
  CHECK(ranged_move.error().code == errc::invalid_argument);
  const auto absent = quarantine(*fs, item_of("log/none", "n"));
  REQUIRE_FALSE(absent.has_value());
  CHECK(absent.error().code == errc::not_found);
  const auto past = quarantine(*fs, ranged("log/seg-00000001.slg", "p", 2, 5));
  CHECK_FALSE(past.has_value());
  CHECK(must(tg::image_of(*fs, "/q")) == before);
}

TEST_CASE("FAB-INV-COR-02: a lenient open quarantines the damaged segment byte for byte and "
          "deletes nothing") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_flip_object(*s.fs, s.dir));
  const bytes_t damaged = read(*s.fs, s.dir / p.file);
  const tg::disk_image before = must(tg::image_of(*s.fs, s.root));
  const checked_open view = must(open_checked(s.root, corruption_mode::lenient, recovery_policy{},
                                              must(tg::sample_options(s.fs, cfg))));
  REQUIRE(view.recovered_view);
  REQUIRE(view.quarantined.size() == 1);
  CHECK(view.quarantined[0].ends_with("-seg-00000001.slg"));
  CHECK(read(*s.fs, s.dir / view.quarantined[0]) == damaged);
  nothing_lost(before, must(tg::image_of(*s.fs, s.root)));

  // A second lenient open quarantines nothing new.
  const checked_open again = must(open_checked(s.root, corruption_mode::lenient, recovery_policy{},
                                               must(tg::sample_options(s.fs, cfg))));
  CHECK(again.quarantined == view.quarantined);
  CHECK(must(s.fs->list(s.dir / "quarantine")).size() == 1);
}

TEST_CASE("FAB-INV-COR-02: an undecodable record object is quarantined as its exact bytes") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const bytes_t garbage = {0xff, 0x10, 0x20};
  const object bad = make_object(object_kind::record, garbage);
  (void)must(tg::plant_record(s, tg::entity_named("e1"), bad, cfg));
  const tg::disk_image before = must(tg::image_of(*s.fs, s.root));
  const checked_open view = must(open_checked(s.root, corruption_mode::lenient, recovery_policy{},
                                              must(tg::sample_options(s.fs, cfg))));
  REQUIRE(view.recovered_view);
  REQUIRE(view.quarantined.size() == 1);
  CHECK(view.quarantined[0] == "quarantine/" + bad.id.to_hex() + "-object");
  const bytes_t kept = read(*s.fs, s.dir / view.quarantined[0]);
  const object reread = must(parse_object(kept));
  CHECK(reread.id == bad.id);
  nothing_lost(before, must(tg::image_of(*s.fs, s.root)));
}
