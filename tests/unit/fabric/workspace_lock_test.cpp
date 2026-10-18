// FAB-INV-RCV-08 (SL-ARCH-002 ARCH2-R34, `recovery_steps` R1, corruption C11): a second writer is
// writer_locked; a stale lock is taken over and reported. On the posix vfs (a scratch directory
// under the ctest working directory) and on vfs_memory: while one workspace holds the OFD lock on
// .slfabric/lock (with its holder text), a second write open is writer_locked and a read-only open
// works without the lock and refuses commits; a clean close empties the lock file and frees the
// lock; a lock file left with a holder's text by a writer that is gone is taken over and reported
// as a non-blocking C11 finding naming the previous holder; a crash releases the lock. create is
// already_exists over a store and open is not_found where there is none.

#include "sl/core/error.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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

std::string read_text(vfs &fs, const std::filesystem::path &p) {
  const std::vector<std::uint8_t> b = must(read_file(fs, p));
  return {b.begin(), b.end()};
}

void write_text(vfs &fs, const std::filesystem::path &p, std::string_view text) {
  auto f = must(fs.open(p, open_create));
  const std::vector<std::uint8_t> b(text.begin(), text.end());
  REQUIRE(f.append(b).has_value());
}

// The lock protocol on any vfs, with the workspace in `root`.
void lock_protocol(const std::shared_ptr<vfs> &fs, const std::filesystem::path &root) {
  const tg::harness_config cfg;
  const tg::crash_workload w = must(tg::make_workload(71, 2, 1, 1));
  const std::filesystem::path lock = root / workspace::store_dir_name / "lock";
  auto writer = must(workspace::create(root, must(tg::harness_options(fs, cfg))));
  {
    auto t = must(writer->manager().begin(tg::harness_tx()));
    REQUIRE(t.stage(w.patches[0]).has_value());
    REQUIRE(must(t.commit()).revision == w.heads[1]);
  }
  CHECK(read_text(*fs, lock).starts_with("slfabric writer pid "));
  CHECK(workspace::create(root, must(tg::harness_options(fs, cfg))).error().code ==
        errc::already_exists);

  // A second writer is refused.
  const auto second =
      workspace::open(root, open_mode::write, cfg.policy, must(tg::harness_options(fs, cfg)));
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().code == errc::writer_locked);
  CHECK(second.error().message.find("lock") != std::string::npos);

  // A reader needs no lock and cannot commit.
  {
    auto reader = must(workspace::open(root, open_mode::read_only, cfg.policy,
                                       must(tg::harness_options(fs, cfg))));
    CHECK(reader->mode() == open_mode::read_only);
    CHECK(must(reader->manager().head()).revision_digest() == w.heads[1]);
    auto t = must(reader->manager().begin(tg::harness_tx()));
    REQUIRE(t.stage(w.patches[1]).has_value());
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::permission_denied);
  }

  // A clean close empties the lock file; the next writer finds no stale holder.
  writer.reset();
  CHECK(read_text(*fs, lock).empty());
  {
    auto next = must(
        workspace::open(root, open_mode::write, cfg.policy, must(tg::harness_options(fs, cfg))));
    CHECK(next->last_recovery().findings.empty());
    CHECK_FALSE(next->last_recovery().stale_lock.has_value());
  }

  // A holder text with no live holder: taken over and reported.
  write_text(*fs, lock, "slfabric writer pid 999999 since 5\n");
  auto taken =
      must(workspace::open(root, open_mode::write, cfg.policy, must(tg::harness_options(fs, cfg))));
  const recovery_report &r = taken->last_recovery();
  CHECK(r.stale_lock == "slfabric writer pid 999999 since 5");
  REQUIRE(r.findings.size() == 1);
  CHECK(r.findings[0].kind == corruption_class::c11);
  CHECK(r.findings[0].code == errc::writer_locked);
  CHECK(r.findings[0].file == "lock");
  CHECK_FALSE(r.findings[0].blocking);
  CHECK(r.findings[0].message.find("pid 999999") != std::string::npos);
  CHECK(read_text(*fs, lock).starts_with("slfabric writer pid "));
  CHECK(read_text(*fs, lock).find("999999") == std::string::npos);
}

} // namespace

TEST_CASE("FAB-INV-RCV-08: a second writer is writer_locked; a stale lock is taken over and "
          "reported (posix vfs)") {
  tg::scratch_dir scratch("fabric_workspace_lock");
  REQUIRE(scratch.ok());
  const auto fs = std::make_shared<vfs_posix>();
  lock_protocol(fs, scratch.path() / "ws");
  const auto none =
      workspace::open(scratch.path() / "nothing", open_mode::write, recovery_policy{});
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == errc::not_found);
}

TEST_CASE("FAB-INV-RCV-08: a second writer is writer_locked; a stale lock is taken over and "
          "reported (memory vfs, crash)") {
  const auto fs = std::make_shared<tg::faulty_vfs>();
  lock_protocol(fs, tg::harness_root());
  const tg::harness_config cfg;
  // A writer that dies keeps nothing locked: a crash releases the OFD lock; under keep_unsynced
  // its holder text survives and the next writer reports it.
  {
    auto writer = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                       must(tg::harness_options(fs, cfg))));
    fs->fail_after(0); // the process dies: its close cannot clear the lock file
  }
  REQUIRE(fs->crash(tg::crash_policy::keep_unsynced).has_value());
  auto next = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                   must(tg::harness_options(fs, cfg))));
  REQUIRE(next->last_recovery().stale_lock.has_value());
  CHECK(next->last_recovery().stale_lock.value_or("").starts_with("slfabric writer pid "));
  CHECK(next->last_recovery().findings.front().kind == corruption_class::c11);
}
