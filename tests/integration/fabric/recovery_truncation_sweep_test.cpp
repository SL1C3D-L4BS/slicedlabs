// FAB-INV-RCV-01 (SL-ARCH-002 ARCH2-R35, R36, R39, R40, `recovery_steps` R3, R4, R6, R8 and R10,
// corruption C1 and C12): truncation at every byte offset of a 40-commit log recovers to the oracle
// after k commits. The log is a seeded workload committed through a durable workspace that is
// reopened every 4 commits, so it holds a checkpoint every 4 commits, and whose segments rotate at
// 12 KiB, so it spans several segment files; the log truncated at offset X is the segment holding
// X cut there with every later segment gone. For every X, recovery keeps exactly the k commits
// whose commit frame survived (its heads are the oracle head after k commits): every checkpoint
// past the cut fails verification (one C12 finding each, moved to quarantine/), the newest one at
// or before it is used, the orphan and torn bytes are cut and reported exactly (lost bytes,
// truncated frames, orphan objects, one C1 finding at a partial frame or a torn segment header,
// none for whole orphan frames) and copied to quarantine/, and a second recovery is lossless. The
// first time each k is reached the whole workspace reopens on the oracle's records. The cases run
// on worker threads, one vfs each; SL_FABRIC_SWEEP_SCALE scales the commit count.

#include "sl/core/error.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

std::optional<digest> main_head(const recovery_report &r) {
  for (const ref_entry &e : r.heads) {
    if (e.name == "main") {
      return e.target;
    }
  }
  return std::nullopt;
}

struct segment_view {
  std::string path;
  std::vector<std::uint8_t> bytes;
  std::vector<tg::frame_entry> frames;
  std::size_t commits_before = 0; // commit frames in earlier segments
};

// What recovery must report for the log cut at `len` bytes into segment `seg`.
struct cut_expectation {
  std::size_t k = 0;
  std::size_t stale_checkpoints = 0; // checkpoints past the cut (C12)
  std::uint64_t used_checkpoint = 0;
  std::uint64_t lost = 0;
  std::uint64_t frames = 0;
  std::uint64_t orphans = 0;
  std::optional<std::uint64_t> torn_at; // the C1 finding's offset
  std::vector<std::uint8_t> cut;        // the bytes quarantined
  std::string kind;                     // "clean", "orphans", "torn", "header"
};

cut_expectation expect_cut(const segment_view &seg, std::uint64_t len) {
  cut_expectation e;
  if (len < segment_header_size) {
    e.k = seg.commits_before;
    e.lost = len;
    e.torn_at = 0;
    e.cut.assign(seg.bytes.begin(), seg.bytes.begin() + static_cast<std::ptrdiff_t>(len));
    e.kind = "header";
    return e;
  }
  std::uint64_t boundary = segment_header_size;
  std::uint64_t commit_end = segment_header_size;
  std::size_t commits = 0;
  for (const tg::frame_entry &f : seg.frames) {
    if (f.end > len) {
      break;
    }
    boundary = f.end;
    if (f.type == frame_type::commit) {
      ++commits;
      commit_end = f.end;
    }
  }
  e.k = seg.commits_before + commits;
  e.lost = len - commit_end;
  for (const tg::frame_entry &f : seg.frames) {
    if (f.offset >= commit_end && f.end <= len) {
      ++e.frames;
      e.orphans += f.type == frame_type::object ? 1U : 0U;
    }
  }
  if (boundary != len) {
    ++e.frames;
    e.torn_at = boundary;
  }
  e.cut.assign(seg.bytes.begin() + static_cast<std::ptrdiff_t>(commit_end),
               seg.bytes.begin() + static_cast<std::ptrdiff_t>(len));
  if (e.torn_at) {
    e.kind = "torn";
  } else {
    e.kind = e.lost == 0 ? "clean" : "orphans";
  }
  return e;
}

// The same with the checkpoints of the log (by seq): those past the cut are C12.
cut_expectation expect(const segment_view &seg, std::uint64_t len,
                       const std::vector<std::uint64_t> &checkpoints) {
  cut_expectation e = expect_cut(seg, len);
  for (const std::uint64_t c : checkpoints) {
    if (c > e.k) {
      ++e.stale_checkpoints;
    } else {
      e.used_checkpoint = std::max(e.used_checkpoint, c);
    }
  }
  return e;
}

// The log cut at `len` bytes into segment `si`: later segments are gone.
tg::disk_image cut_image(const tg::disk_image &image, const std::vector<segment_view> &segments,
                         std::size_t si, std::uint64_t len) {
  tg::disk_image cut = image;
  cut.files[segments[si].path].resize(static_cast<std::size_t>(len));
  for (std::size_t later = si + 1; later < segments.size(); ++later) {
    cut.files.erase(segments[later].path);
  }
  return cut;
}

// One case; "" when every cut_expectation holds.
std::string run_case(const tg::disk_image &image, const std::vector<segment_view> &segments,
                     std::size_t si, std::uint64_t len,
                     const std::vector<std::uint64_t> &checkpoints, const tg::crash_workload &w,
                     const tg::harness_config &cfg) {
  const cut_expectation e = expect(segments[si], len, checkpoints);
  auto fs = tg::restore_memory(cut_image(image, segments, si, len));
  if (!fs) {
    return "restore: " + fs.error().message;
  }
  const auto rec = recover_log(**fs, tg::harness_store(), cfg.policy, cfg.log);
  if (!rec) {
    return "recover_log: " + rec.error().message;
  }
  const recovery_report &r = rec->report;
  std::string bad;
  const auto want = [&bad](bool ok, const std::string &what) {
    if (!ok && bad.empty()) {
      bad = what;
    }
  };
  want(!r.blocking(), "blocking: " + r.summary());
  want(main_head(r) == w.heads[e.k], "head is not the oracle head after " + std::to_string(e.k));
  want(r.last_seq == e.k, "last seq " + std::to_string(r.last_seq));
  want(r.lost_bytes == e.lost,
       "lost bytes " + std::to_string(r.lost_bytes) + ", expected " + std::to_string(e.lost));
  want(r.truncated_frames == e.frames, "truncated frames " + std::to_string(r.truncated_frames) +
                                           ", expected " + std::to_string(e.frames));
  want(r.orphan_objects == e.orphans, "orphan objects " + std::to_string(r.orphan_objects) +
                                          ", expected " + std::to_string(e.orphans));
  want(r.used_checkpoint == (e.used_checkpoint != 0) && r.checkpoint_seq == e.used_checkpoint,
       "used checkpoint " + std::to_string(r.checkpoint_seq) + ", expected " +
           std::to_string(e.used_checkpoint));
  const std::size_t findings = e.stale_checkpoints + (e.torn_at ? 1U : 0U);
  want(r.findings.size() == findings,
       "expected " + std::to_string(findings) + " findings: " + r.summary());
  for (std::size_t i = 0; i < r.findings.size() && i < e.stale_checkpoints; ++i) {
    want(r.findings[i].kind == corruption_class::c12 && !r.findings[i].blocking,
         "expected C12 for a checkpoint past the cut: " + r.summary());
  }
  if (e.torn_at && !r.findings.empty()) {
    const recovery_finding &f = r.findings.back();
    want(f.kind == corruption_class::c1 && f.offset == *e.torn_at && f.seq == e.k,
         "expected C1 at offset " + std::to_string(*e.torn_at) + ": " + r.summary());
  }
  const std::size_t tail = e.cut.empty() ? 0U : 1U;
  want(r.quarantined.size() == tail + e.stale_checkpoints,
       "quarantined " + std::to_string(r.quarantined.size()) + " files");
  if (tail == 1 && !r.quarantined.empty()) {
    const auto q = read_file(**fs, tg::harness_store() / r.quarantined[0]);
    want(q && *q == e.cut, "the quarantined bytes are not the cut bytes");
  }
  const auto again = recover_log(**fs, tg::harness_store(), cfg.policy, cfg.log);
  want(again && again->report.lossless() && main_head(again->report) == w.heads[e.k],
       "the second recovery is not lossless");
  return bad;
}

} // namespace

TEST_CASE("FAB-INV-RCV-01: truncation at every byte offset of a 40-commit log recovers to the "
          "oracle after k commits") {
  tg::harness_config cfg;
  cfg.log.rotate_at = std::uint64_t{12} * 1024;
  cfg.log.loose_above = 1U << 20U;
  cfg.policy.open_store = false;
  const std::size_t commits = tg::scaled(40, 4);
  const tg::crash_workload w = must(tg::make_workload(11, commits, 1, 1));
  // The log: a reopen every 4 commits writes a checkpoint (all kept).
  constexpr std::size_t checkpoint_every = 4;
  auto built = std::make_shared<tg::faulty_vfs>();
  {
    tg::harness_config build = cfg;
    build.policy.open_store = true;
    build.policy.keep_checkpoints = 1000;
    auto ws = must(workspace::create(tg::harness_root(), must(tg::harness_options(built, build))));
    for (std::size_t i = 0; i < commits; ++i) {
      if (i > 0 && i % checkpoint_every == 0) {
        ws.reset();
        ws = must(workspace::open(tg::harness_root(), open_mode::write, build.policy,
                                  must(tg::harness_options(built, build))));
        REQUIRE(ws->last_recovery().checkpoint_written == i);
      }
      auto t = must(ws->manager().begin(tg::harness_tx()));
      REQUIRE(t.stage(w.patches[i]).has_value());
      REQUIRE(must(t.commit()).revision == w.heads[i + 1]);
    }
  }
  const tg::disk_image image = must(tg::image_of(*built->memory(), tg::harness_root()));
  std::vector<std::uint64_t> checkpoints;
  for (const std::string &name : must(list_checkpoints(*built, tg::harness_store()))) {
    checkpoints.push_back(parse_checkpoint_file_name(name).value_or(0));
  }
  REQUIRE(checkpoints.size() == (commits - 1) / checkpoint_every);

  std::vector<segment_view> segments;
  std::size_t seen = 0;
  for (const auto &[path, bytes] : image.files) {
    if (!path.contains("/log/seg-")) {
      continue;
    }
    segment_view s{
        .path = path, .bytes = bytes, .frames = tg::frame_table(bytes), .commits_before = seen};
    seen += static_cast<std::size_t>(std::ranges::count_if(
        s.frames, [](const tg::frame_entry &f) { return f.type == frame_type::commit; }));
    segments.push_back(std::move(s));
  }
  REQUIRE(seen == commits);
  if (commits >= 40) {
    REQUIRE(segments.size() >= 3);
  }

  // Every offset of the log, then the whole log.
  std::vector<std::pair<std::size_t, std::uint64_t>> cases;
  for (std::size_t si = 0; si < segments.size(); ++si) {
    for (std::uint64_t len = 0; len < segments[si].bytes.size(); ++len) {
      cases.emplace_back(si, len);
    }
  }
  cases.emplace_back(segments.size() - 1, segments.back().bytes.size());

  std::vector<std::string> failures(cases.size());
  const std::size_t workers = tg::sweep_threads();
  tg::parallel_for(cases.size(), [&](std::size_t i) {
    failures[i] = run_case(image, segments, cases[i].first, cases[i].second, checkpoints, w, cfg);
  });

  std::map<std::string, std::size_t> kinds;
  std::size_t failed = 0;
  std::set<std::size_t> opened;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto [si, len] = cases[i];
    const cut_expectation e = expect(segments[si], len, checkpoints);
    ++kinds[e.kind];
    if (!failures[i].empty()) {
      ++failed;
      if (failed <= 20) {
        CAPTURE(segments[si].path);
        CAPTURE(len);
        CHECK_MESSAGE(failures[i].empty(), failures[i]);
      }
    }
    if (opened.insert(e.k).second) {
      // The first cut that recovers to k: the whole workspace reopens on the oracle's records.
      CAPTURE(e.k);
      auto fs = must(tg::restore(cut_image(image, segments, si, len)));
      tg::harness_config full = cfg;
      full.policy.open_store = true;
      auto ws = must(workspace::open(tg::harness_root(), open_mode::write, full.policy,
                                     must(tg::harness_options(fs, full))));
      CHECK(must(ws->manager().head()).revision_digest() == w.heads[e.k]);
      CHECK(must(tg::oracle_of(must(ws->manager().head()))) == w.states[e.k]);
    }
  }
  CHECK(failed == 0);
  CHECK(opened.size() == commits + 1);
  MESSAGE("truncation sweep: " << cases.size() << " offsets over " << segments.size()
                               << " segments on " << workers << " threads: clean " << kinds["clean"]
                               << ", orphans only " << kinds["orphans"] << ", torn frame "
                               << kinds["torn"] << ", torn header " << kinds["header"] << "; "
                               << checkpoints.size() << " checkpoints; failed " << failed);
  CHECK(kinds["torn"] > 0);
  CHECK(kinds["orphans"] > 0);
  CHECK(kinds["header"] > 0);
}
