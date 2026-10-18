// FAB-INV-RCV-10 (SL-ARCH-002 ARCH2-R38, ADR-0015): an indeterminate commit blocks commits, is
// decided on reopen, and a keyed retry returns the receipt. A commit whose segment fsync fails
// after its append (FP4) is commit_indeterminate: the manager refuses every later begin with
// recovery_required. Reopening decides the commit by the intactness of its commit frame: when its
// written bytes survived (a process crash, or a power loss that kept the whole append) the frame
// is intact, the commit stands and the keyed retry returns the original receipt without writing;
// after a power loss (drop_unsynced) or a torn append the commit is gone (a torn tail is cut, C1)
// and the keyed retry commits it once, to the same revision. A failure after the fsync (FP5) is
// decided the same way.

#include "sl/core/error.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

sl::result<commit_receipt> commit(workspace &ws, const patch &p,
                                  const std::optional<digest> &base = std::nullopt) {
  tx_options o = tg::harness_tx();
  o.base = base;
  auto t = ws.manager().begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  if (auto ok = t->stage(p); !ok) {
    return std::unexpected(ok.error());
  }
  return t->commit();
}

struct indeterminate {
  std::shared_ptr<tg::faulty_vfs> fs;
  std::unique_ptr<workspace> ws;
};

// Commit 1 lands; commit 2 fails at its segment fsync: commit_indeterminate.
indeterminate at_fsync(const tg::crash_workload &w, const tg::harness_config &cfg) {
  for (std::uint64_t k = 0; k < 64; ++k) {
    indeterminate out{.fs = std::make_shared<tg::faulty_vfs>(), .ws = nullptr};
    out.ws = must(workspace::create(tg::harness_root(), must(tg::harness_options(out.fs, cfg))));
    REQUIRE(must(commit(*out.ws, w.patches[0])).revision == w.heads[1]);
    out.fs->fail_after(k);
    const auto r = commit(*out.ws, w.patches[1]);
    if (r) {
      FAIL("no primitive of commit 2 made it indeterminate");
    }
    if (r.error().code == errc::commit_indeterminate) {
      REQUIRE(out.fs->trace().back().starts_with("fsync seg-"));
      return out;
    }
    CHECK(r.error().code == errc::store_io);
    CHECK_FALSE(out.ws->manager().recovery_required());
  }
  FAIL("commit 2 never became indeterminate");
  return {};
}

void blocked(workspace &ws) {
  CHECK(ws.manager().recovery_required());
  const auto t = ws.manager().begin(tg::harness_tx());
  REQUIRE_FALSE(t.has_value());
  CHECK(t.error().code == errc::recovery_required);
}

std::unique_ptr<workspace> reopen(const std::shared_ptr<tg::faulty_vfs> &fs,
                                  const tg::harness_config &cfg) {
  return must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                              must(tg::harness_options(fs, cfg))));
}

} // namespace

TEST_CASE("FAB-INV-RCV-10: an intact commit frame decides an indeterminate commit as committed; "
          "the keyed retry returns the receipt") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  const tg::crash_workload w = must(tg::make_workload(81, 3, 1, 1));
  for (const std::uint64_t keep : {std::uint64_t{0}, std::uint64_t{1} << 30U}) {
    CAPTURE(keep);
    indeterminate s = at_fsync(w, cfg);
    blocked(*s.ws);
    s.ws.reset(); // the process dies; its append reached the file, the sync did not finish
    // keep_unsynced: a process crash keeps the written bytes; a tear past the whole append is
    // the same for a power loss that happened to keep them.
    REQUIRE(s.fs->crash(keep == 0 ? tg::crash_policy::keep_unsynced
                                  : tg::crash_policy::tear_last_append,
                        keep)
                .has_value());
    auto ws = reopen(s.fs, cfg);
    CHECK(ws->last_recovery().lost_bytes == 0);
    CHECK(ws->last_recovery().last_seq == 2);
    CHECK(must(ws->manager().head()).revision_digest() == w.heads[2]);
    CHECK_FALSE(ws->manager().recovery_required());
    const commit_receipt retry = must(commit(*ws, w.patches[1], w.heads[1]));
    CHECK(retry.recovered);
    CHECK(retry.revision == w.heads[2]);
    CHECK(ws->last_recovery().last_seq == 2);
    CHECK(must(commit(*ws, w.patches[2])).revision == w.heads[3]);
  }
}

TEST_CASE("FAB-INV-RCV-10: a lost or torn commit frame decides it as not committed; the keyed "
          "retry commits it once") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  const tg::crash_workload w = must(tg::make_workload(82, 3, 1, 1));
  for (const std::uint64_t tear : {std::uint64_t{0}, std::uint64_t{20}}) {
    CAPTURE(tear);
    indeterminate s = at_fsync(w, cfg);
    blocked(*s.ws);
    s.ws.reset();
    REQUIRE(s.fs->crash(tear == 0 ? tg::crash_policy::drop_unsynced
                                  : tg::crash_policy::tear_last_append,
                        tear)
                .has_value());
    auto ws = reopen(s.fs, cfg);
    const recovery_report &r = ws->last_recovery();
    CHECK(r.last_seq == 1);
    if (tear != 0) {
      CHECK(r.lost_bytes == tear);
      REQUIRE_FALSE(r.findings.empty());
      CHECK(r.findings.back().kind == corruption_class::c1);
    }
    CHECK(must(ws->manager().head()).revision_digest() == w.heads[1]);
    const commit_receipt retry = must(commit(*ws, w.patches[1], w.heads[1]));
    CHECK_FALSE(retry.recovered);
    CHECK(retry.revision == w.heads[2]);
    const commit_receipt again = must(commit(*ws, w.patches[1], w.heads[1]));
    CHECK(again.revision == w.heads[2]);
    CHECK(again.recovered == false);
    CHECK(again == retry); // the original receipt, not a second commit
  }
}

TEST_CASE("FAB-INV-RCV-10: a failure after the fsync (FP5) is decided as committed on reopen") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  const tg::crash_workload w = must(tg::make_workload(83, 2, 1, 1));
  auto fs = std::make_shared<tg::faulty_vfs>();
  auto ws = must(workspace::create(tg::harness_root(), must(tg::harness_options(fs, cfg))));
  REQUIRE(must(commit(*ws, w.patches[0])).revision == w.heads[1]);
  REQUIRE(tg::tx_failpoints::arm(ws->manager().failpoints(), failpoint::fp5).has_value());
  const auto r = commit(*ws, w.patches[1]);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::commit_indeterminate);
  blocked(*ws);
  ws.reset();
  REQUIRE(fs->crash(tg::crash_policy::drop_unsynced).has_value());
  ws = reopen(fs, cfg);
  CHECK(must(ws->manager().head()).revision_digest() == w.heads[2]);
  const commit_receipt retry = must(commit(*ws, w.patches[1], w.heads[1]));
  CHECK(retry.recovered);
  CHECK(retry.revision == w.heads[2]);
}
