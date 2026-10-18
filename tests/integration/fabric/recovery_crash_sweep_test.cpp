// FAB-INV-RCV-09 (SL-ARCH-002 ARCH2-R35 to R38, ADR-0015): a crash after every storage primitive
// of seeded workloads loses no acknowledged commit and recovers to the oracle. Each workload
// creates a durable workspace on a faulty vfs over vfs_memory (segments rotate at 4 KiB, objects
// above 512 bytes go loose) and commits generated patches. For every primitive k of the
// fault-free run, the run is repeated with primitive k failing (the process dies there: every
// later primitive fails too), then crashed under drop_unsynced, keep_unsynced and
// tear_last_append at several tear offsets, then reopened with recovery. The reopen must find the
// head after exactly the acknowledged commits, or one more when a commit was in flight (FP3 a
// failed segment append, FP4 a failed sync after it), with the oracle's records; a second reopen
// must report no loss and the same head; a keyed retry of the commit in flight converges and
// returns the original receipt when it was durable; the next commit of the workload then lands on
// the oracle's next head. The manager failpoints FP0, FP1, FP2, FP5 and FP6 are crossed with every
// crash policy at every commit, and a crash at every primitive of a recovery itself recovers to the
// same head. SL_FABRIC_SWEEP_SCALE scales the seeds; SL_FABRIC_SOAK_SEED (just fabric-soak)
// replaces them with one random seed.

#include "sl/core/error.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

struct crash_mode {
  tg::crash_policy policy;
  std::uint64_t tear;
  const char *name;
};

const std::vector<crash_mode> &crash_modes() {
  static const std::vector<crash_mode> modes = {
      {.policy = tg::crash_policy::drop_unsynced, .tear = 0, .name = "drop_unsynced"},
      {.policy = tg::crash_policy::keep_unsynced, .tear = 0, .name = "keep_unsynced"},
      {.policy = tg::crash_policy::tear_last_append, .tear = 0, .name = "tear@0"},
      {.policy = tg::crash_policy::tear_last_append, .tear = 5, .name = "tear@5"},
      {.policy = tg::crash_policy::tear_last_append, .tear = 31, .name = "tear@31"},
      {.policy = tg::crash_policy::tear_last_append, .tear = 700, .name = "tear@700"}};
  return modes;
}

// The crash modes that can differ for a run: a failure on a read leaves the disk as the previous
// primitive did (that state is swept at the next mutating primitive under every mode), and the
// tear modes equal drop_unsynced unless the last append was not synced.
bool mode_matters(const crash_mode &mode, const tg::crash_run &run) {
  if (run.failed_on_read) {
    return mode.policy == tg::crash_policy::drop_unsynced;
  }
  return mode.policy != tg::crash_policy::tear_last_append || run.unsynced_append;
}

std::vector<std::uint64_t> seeds(std::size_t count) {
  if (const std::optional<std::uint64_t> s = tg::soak_seed()) {
    return {*s};
  }
  std::vector<std::uint64_t> out;
  const std::size_t n = tg::scaled(count);
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(1 + (i * 7919));
  }
  return out;
}

// Tallies for the summary line.
struct tally {
  std::size_t cases = 0;
  std::size_t old_head = 0;
  std::size_t new_head = 0;
  std::size_t fp3 = 0;
  std::size_t fp4 = 0;
  std::size_t created_again = 0;
  std::map<std::string, std::size_t> classes; // findings of the first reopen by class
  std::size_t lost_bytes = 0;
  std::size_t orphans = 0;
  std::size_t quarantined = 0;

  void add(const tg::crash_run &run, const tg::recovery_check &c) {
    ++cases;
    (c.recovered > run.acked ? new_head : old_head) += 1;
    created_again += c.created_again ? 1U : 0U;
    if (run.failed_primitive.starts_with("append seg-")) {
      ++fp3;
    }
    if (run.failed_primitive.starts_with("fsync seg-") ||
        run.failed_primitive.starts_with("fdatasync seg-")) {
      ++fp4;
    }
    for (const recovery_finding &f : c.first.findings) {
      ++classes[std::string(to_string(f.kind))];
    }
    lost_bytes += c.first.lost_bytes;
    orphans += c.first.orphan_objects;
    quarantined += c.first.quarantined.size();
  }

  [[nodiscard]] std::string line(const char *what) const {
    std::string s = std::string(what) + ": " + std::to_string(cases) + " cases, old head " +
                    std::to_string(old_head) + ", new head " + std::to_string(new_head) + ", FP3 " +
                    std::to_string(fp3) + ", FP4 " + std::to_string(fp4) + ", creation redone " +
                    std::to_string(created_again) + ", lost bytes " + std::to_string(lost_bytes) +
                    ", orphans " + std::to_string(orphans) + ", quarantined " +
                    std::to_string(quarantined) + ", findings";
    for (const auto &[cls, n] : classes) {
      s += " " + cls + "=" + std::to_string(n);
    }
    return s;
  }
};

// One crash case: what ran, what the reopen found.
struct case_result {
  std::string label;
  tg::crash_run run;
  tg::recovery_check check;
  std::string error; // a harness failure (not a recovery violation)
};

// Every mode that matters for one fault position; `run_one` runs the faulted workload on a fresh
// vfs and returns it (the caller crashes it).
template <class RunOne>
std::vector<case_result> sweep_modes(const tg::crash_workload &w, const tg::harness_config &cfg,
                                     const std::string &label, RunOne run_one) {
  std::vector<case_result> out;
  std::optional<tg::crash_run> first;
  for (const crash_mode &mode : crash_modes()) {
    if (first && !mode_matters(mode, *first)) {
      continue;
    }
    case_result r;
    r.label = label + " " + mode.name;
    auto [fs, run] = run_one();
    first = run;
    r.run = run;
    if (!fs->crash(mode.policy, mode.tear)) {
      r.error = "crash failed";
    } else {
      r.check = tg::check_recovery(fs, w, cfg, run);
    }
    out.push_back(std::move(r));
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-RCV-09: a crash after every storage primitive of seeded workloads loses no "
          "acknowledged commit and recovers to the oracle") {
  const tg::harness_config cfg;
  std::vector<tg::crash_workload> workloads;
  std::vector<std::pair<std::size_t, std::uint64_t>> units; // (workload, primitive k)
  for (const std::uint64_t seed : seeds(3)) {
    CAPTURE(seed);
    workloads.push_back(must(tg::make_workload(seed, 8)));
    const tg::crash_run clean =
        tg::run_workload(std::make_shared<tg::faulty_vfs>(), workloads.back(), cfg, {});
    REQUIRE_MESSAGE(!clean.failure, clean.failure->message);
    REQUIRE(clean.acked == workloads.back().patches.size());
    for (std::uint64_t k = 0; k < clean.primitives; ++k) {
      units.emplace_back(workloads.size() - 1, k);
    }
  }
  std::vector<std::vector<case_result>> results(units.size());
  tg::parallel_for(units.size(), [&](std::size_t u) {
    const auto [wi, k] = units[u];
    const tg::crash_workload &w = workloads[wi];
    results[u] =
        sweep_modes(w, cfg, "seed " + std::to_string(w.seed) + " k " + std::to_string(k), [&] {
          auto fs = std::make_shared<tg::faulty_vfs>();
          tg::crash_run run = tg::run_workload(fs, w, cfg, k);
          return std::make_pair(fs, run);
        });
  });
  tally t;
  for (const auto &unit : results) {
    for (const case_result &r : unit) {
      CAPTURE(r.label);
      CAPTURE(r.run.failed_primitive);
      REQUIRE_MESSAGE(r.error.empty(), r.error);
      CHECK_MESSAGE(r.check.violation.empty(), r.check.violation);
      t.add(r.run, r.check);
    }
  }
  MESSAGE(t.line("primitive crash sweep"));
  CHECK(t.fp3 > 0);
  CHECK(t.fp4 > 0);
  CHECK(t.new_head > 0);
  CHECK(t.classes["C1"] > 0);
}

TEST_CASE("FAB-INV-RCV-09: every manager failpoint at every commit under every crash policy "
          "leaves the old or the new head and converges") {
  const tg::harness_config cfg;
  const std::vector<failpoint> fps = {failpoint::fp0, failpoint::fp1, failpoint::fp2,
                                      failpoint::fp5, failpoint::fp6};
  std::vector<tg::crash_workload> workloads;
  struct unit {
    std::size_t workload;
    std::size_t commit;
    failpoint fp;
  };
  std::vector<unit> units;
  for (const std::uint64_t seed : seeds(3)) {
    workloads.push_back(must(tg::make_workload(seed, 5)));
    for (std::size_t commit = 0; commit < workloads.back().patches.size(); ++commit) {
      for (const failpoint fp : fps) {
        units.push_back(unit{.workload = workloads.size() - 1, .commit = commit, .fp = fp});
      }
    }
  }
  std::vector<std::vector<case_result>> results(units.size());
  tg::parallel_for(units.size(), [&](std::size_t i) {
    const unit &u = units[i];
    const tg::crash_workload &w = workloads[u.workload];
    results[i] =
        sweep_modes(w, cfg,
                    "seed " + std::to_string(w.seed) + " commit " + std::to_string(u.commit) + " " +
                        std::string(to_string(u.fp)),
                    [&] {
                      auto fs = std::make_shared<tg::faulty_vfs>();
                      tg::crash_run run = tg::run_workload(
                          fs, w, cfg, {}, tg::armed_failpoint{.commit = u.commit, .fp = u.fp});
                      return std::make_pair(fs, run);
                    });
  });
  tally t;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const bool durable = units[i].fp == failpoint::fp5 || units[i].fp == failpoint::fp6;
    for (const case_result &r : results[i]) {
      CAPTURE(r.label);
      REQUIRE_MESSAGE(r.error.empty(), r.error);
      REQUIRE(r.run.tripped == units[i].fp);
      REQUIRE(r.run.acked == units[i].commit);
      REQUIRE(r.run.failure.has_value());
      CHECK((r.run.failure &&
             r.run.failure->code == (durable ? errc::commit_indeterminate : errc::store_io)));
      CHECK_MESSAGE(r.check.violation.empty(), r.check.violation);
      // FP0-FP2 never reach the commit point; FP5 and FP6 are past the fsync.
      CHECK(r.check.recovered == units[i].commit + (durable ? 1U : 0U));
      t.add(r.run, r.check);
    }
  }
  MESSAGE(t.line("failpoint sweep"));
}

TEST_CASE("FAB-INV-RCV-09: a crash at every primitive of a recovery recovers to the same head") {
  const tg::harness_config cfg;
  for (const std::uint64_t seed : seeds(2)) {
    CAPTURE(seed);
    const tg::crash_workload w = must(tg::make_workload(seed, 6));
    const tg::crash_run clean = tg::run_workload(std::make_shared<tg::faulty_vfs>(), w, cfg, {});
    REQUIRE(!clean.failure);
    // A dirty store: the last commit whose sync failed, torn 40 bytes into its append.
    std::optional<tg::disk_image> dirty;
    std::size_t acked = 0;
    for (std::uint64_t k = clean.primitives; k-- > 0 && !dirty;) {
      auto fs = std::make_shared<tg::faulty_vfs>();
      const tg::crash_run run = tg::run_workload(fs, w, cfg, k);
      if (!run.failed_primitive.starts_with("fsync seg-")) {
        continue;
      }
      REQUIRE(fs->crash(tg::crash_policy::tear_last_append, 40).has_value());
      dirty = must(tg::image_of(*fs->memory(), tg::harness_root()));
      acked = run.acked;
    }
    REQUIRE(dirty.has_value());
    // The fault-free recovery of that store.
    std::size_t want = 0;
    std::uint64_t recovery_primitives = 0;
    {
      auto fs = must(tg::restore(*dirty));
      const std::uint64_t before = fs->primitives();
      auto ws = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                     must(tg::harness_options(fs, cfg))));
      recovery_primitives = fs->primitives() - before;
      CHECK(ws->last_recovery().lost_bytes > 0);
      const auto head = must(ws->manager().head()).revision_digest();
      want = static_cast<std::size_t>(std::ranges::find(w.heads, head) - w.heads.begin());
      CHECK(want == acked);
    }
    std::vector<std::vector<case_result>> results(recovery_primitives);
    tg::parallel_for(recovery_primitives, [&](std::size_t k) {
      results[k] = sweep_modes(w, cfg, "recovery k " + std::to_string(k), [&] {
        auto fs = tg::restore(*dirty).value_or(nullptr);
        tg::crash_run as_run;
        as_run.created = true;
        as_run.acked = want;
        if (!fs) {
          return std::make_pair(std::make_shared<tg::faulty_vfs>(), as_run);
        }
        fs->fail_after(k);
        {
          // The process dies somewhere in the open; whatever it managed to do stays.
          auto o = tg::harness_options(fs, cfg);
          if (o) {
            auto ws =
                workspace::open(tg::harness_root(), open_mode::write, cfg.policy, std::move(*o));
          }
        }
        tg::note_failure(fs->trace(), as_run);
        return std::make_pair(fs, as_run);
      });
    });
    std::size_t cases = 0;
    for (const auto &unit : results) {
      for (const case_result &r : unit) {
        CAPTURE(r.label);
        REQUIRE_MESSAGE(r.error.empty(), r.error);
        CHECK_MESSAGE(r.check.violation.empty(), r.check.violation);
        CHECK(r.check.recovered == want);
        ++cases;
      }
    }
    MESSAGE("recovery crash sweep seed " << seed << ": " << recovery_primitives
                                         << " recovery primitives, " << cases << " cases");
  }
}
