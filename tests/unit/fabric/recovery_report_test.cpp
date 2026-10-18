// FAB-INV-RCV-07 (SL-ARCH-002 ARCH2-R40): the recovery report fields equal the oracle exactly.
// Planted stores whose outcome is known byte for byte: a torn frame after the last commit of a
// store with two refs, an open intent and a stale lock; a torn segment header after a rotation;
// and the clean reopen after each. Every field is compared with the value computed from the
// frame table and the workload: lost bytes, truncated frames, orphan objects, open intents with
// options, per-ref heads, checkpoint use, quarantined files and their bytes, findings with class,
// code, file, offset, seq and blocking, the scan counters and the summary text.

#include "sl/core/error.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

using bytes_t = std::vector<std::uint8_t>;

bytes_t text(std::string_view s) { return {s.begin(), s.end()}; }

void same_finding(const recovery_finding &got, const recovery_finding &want) {
  CHECK(got.kind == want.kind);
  CHECK(got.code == want.code);
  CHECK(got.file == want.file);
  CHECK(got.offset == want.offset);
  CHECK(got.seq == want.seq);
  CHECK(got.commits_after == want.commits_after);
  CHECK(got.expected == want.expected);
  CHECK(got.found == want.found);
  CHECK(got.blocking == want.blocking);
  CHECK(got.message.starts_with(want.message));
}

} // namespace

TEST_CASE("FAB-INV-RCV-07: a torn frame in a store with two refs, an open intent and a stale "
          "lock is reported exactly") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.log.rotate_at = 1U << 20U;
  const tg::crash_workload w = must(tg::make_workload(61, 3, 1, 1));
  auto fs = std::make_shared<tg::faulty_vfs>();
  REQUIRE(!tg::run_workload(fs, w, cfg, {}).failure);
  std::vector<ref_entry> heads;
  {
    auto ws = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                   must(tg::harness_options(fs, cfg))));
    REQUIRE(ws->last_recovery().checkpoint_written == 3);
    (void)must(ws->manager().create_ref("review", w.heads[3].value_or(digest{}),
                                        tg::tx_origin_of(tg::tx_editor())));
    heads = ws->manager().refs();
  }
  REQUIRE(heads.size() == 2);
  const record_id x = tg::operation_named("report/x");
  REQUIRE(must(tg::commit_intents(*fs, cfg.log,
                                  {intent_record{.operation = x,
                                                 .kind = "import",
                                                 .phase = intent_phase::begin,
                                                 .outcome = intent_outcome::none}})) == 5);
  tg::disk_image img = must(tg::image_of(*fs->memory(), tg::harness_root()));
  img.files[(tg::harness_store() / "lock").generic_string()] = text("slfabric writer pid 7\n");
  bytes_t &seg = img.files[tg::segment_path(1)];
  const std::vector<tg::frame_entry> frames = tg::frame_table(seg);
  const std::uint64_t cut = seg.size();
  const bytes_t torn = must(encode_frame(frame_type::note, text("a note that never finished")));
  seg.insert(seg.end(), torn.begin(), torn.begin() + 30);
  std::uint64_t ckpt_end = 0;
  std::size_t commits = 0;
  for (const tg::frame_entry &f : frames) {
    if (f.type == frame_type::commit && ++commits == 3) {
      ckpt_end = f.end;
    }
  }
  std::uint64_t delta_frames = 0;
  for (const tg::frame_entry &f : frames) {
    delta_frames += f.offset >= ckpt_end ? 1U : 0U;
  }

  auto disk = must(tg::restore(img));
  auto ws = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                 must(tg::harness_options(disk, cfg))));
  const recovery_report &r = ws->last_recovery();
  CHECK(r.lost_bytes == 30);
  CHECK(r.truncated_frames == 1);
  CHECK(r.orphan_objects == 0);
  REQUIRE(r.open_intents.size() == 1);
  CHECK(r.open_intents[0] ==
        open_intent{.operation = x, .kind = "import", .options = {"continue", "compensate"}});
  CHECK(r.heads == heads);
  CHECK(r.used_checkpoint);
  CHECK(r.checkpoint_seq == 3);
  const std::string qname = "quarantine/5-tail-" + segment_file_name(1) + "-" + std::to_string(cut);
  CHECK(r.quarantined == std::vector<std::string>{qname});
  CHECK(must(read_file(*disk, tg::harness_store() / qname)) ==
        bytes_t(torn.begin(), torn.begin() + 30));
  REQUIRE(r.findings.size() == 2);
  same_finding(r.findings[0], recovery_finding{.kind = corruption_class::c11,
                                               .code = errc::writer_locked,
                                               .file = "lock",
                                               .offset = 0,
                                               .seq = 0,
                                               .commits_after = 0,
                                               .expected = std::nullopt,
                                               .found = std::nullopt,
                                               .blocking = false,
                                               .message = "C11 stale lock: lock:"});
  CHECK(r.findings[0].message.find("slfabric writer pid 7") != std::string::npos);
  same_finding(r.findings[1],
               recovery_finding{.kind = corruption_class::c1,
                                .code = errc::journal_corrupt,
                                .file = "log/" + segment_file_name(1),
                                .offset = cut,
                                .seq = 5,
                                .commits_after = 0,
                                .expected = std::nullopt,
                                .found = std::nullopt,
                                .blocking = false,
                                .message = "C1 torn tail: log/" + segment_file_name(1) +
                                           " offset " + std::to_string(cut) + ":"});
  CHECK(r.last_seq == 5);
  CHECK(r.commits_scanned == 2);
  CHECK(r.frames_scanned == delta_frames);
  CHECK(r.bytes_scanned == cut + 30 - ckpt_end);
  CHECK(r.checkpoint_written == 5);
  CHECK(r.stale_lock == "slfabric writer pid 7");
  CHECK_FALSE(r.blocking());
  CHECK_FALSE(r.lossless());
  const std::string summary = r.summary();
  CHECK(summary.starts_with("recovery: last seq 5, lost 30 bytes, 1 frames truncated, 0 orphan "
                            "objects, 1 open intents, 2 findings"));
  CHECK(summary.find("\n  C11 stale lock") != std::string::npos);
  CHECK(summary.find("\n  C1 torn tail") != std::string::npos);
  CHECK(must(read_file(*disk, tg::segment_path(1))).size() == cut);
  const std::vector<open_intent> intents = r.open_intents;
  ws.reset(); // r dies with the workspace

  auto again = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                    must(tg::harness_options(disk, cfg))));
  const recovery_report &r2 = again->last_recovery();
  CHECK(r2.lossless());
  CHECK(r2.heads == heads);
  CHECK(r2.open_intents == intents);
  CHECK(r2.checkpoint_seq == 5);
  CHECK(r2.frames_scanned == 0);
  CHECK(r2.bytes_scanned == 0);
  CHECK(r2.checkpoint_written == 0);
  CHECK_FALSE(r2.stale_lock.has_value());
  CHECK(r2.summary() == "recovery: last seq 5, lost 0 bytes, 0 frames truncated, 0 orphan "
                        "objects, 1 open intents, 0 findings");
}

TEST_CASE("FAB-INV-RCV-07: a torn segment header after a rotation is reported exactly") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.log.rotate_at = 2500;
  const tg::built_log log = must(tg::build_log(62, 6, cfg));
  std::uint32_t segments = 0;
  std::size_t before_last = 0;
  for (std::uint32_t n = 1; log.image.files.contains(tg::segment_path(n)); ++n) {
    segments = n;
  }
  REQUIRE(segments >= 2);
  for (std::uint32_t n = 1; n <= segments; ++n) {
    for (const tg::frame_entry &f : tg::frame_table(log.image.files.at(tg::segment_path(n)))) {
      before_last += f.type == frame_type::commit ? 1U : 0U;
    }
  }
  // A rotation to segment segments + 1 stopped 11 bytes into its header.
  tg::disk_image img = log.image;
  const auto header = encode_segment_header(segment_header{
      .major = 1, .minor = 0, .segment = segments + 1, .first_seq = before_last + 1});
  img.files[tg::segment_path(segments + 1)] = bytes_t(header.begin(), header.begin() + 11);
  auto fs = must(tg::restore(img));
  const recovered_log rec = must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log));
  const recovery_report &r = rec.report;
  REQUIRE(rec.store);
  CHECK(r.lost_bytes == 11);
  CHECK(r.truncated_frames == 0);
  CHECK(r.orphan_objects == 0);
  CHECK(r.open_intents.empty());
  CHECK(r.heads == std::vector<ref_entry>{ref_entry{
                       .name = "main", .target = log.workload.heads[6].value_or(digest{})}});
  CHECK_FALSE(r.used_checkpoint);
  const std::string qname =
      "quarantine/" + std::to_string(before_last) + "-header-" + segment_file_name(segments + 1);
  CHECK(r.quarantined == std::vector<std::string>{qname});
  CHECK(must(read_file(*fs, tg::harness_store() / qname)) ==
        bytes_t(header.begin(), header.begin() + 11));
  REQUIRE(r.findings.size() == 1);
  same_finding(r.findings[0],
               recovery_finding{.kind = corruption_class::c1,
                                .code = errc::journal_corrupt,
                                .file = "log/" + segment_file_name(segments + 1),
                                .offset = 0,
                                .seq = before_last,
                                .commits_after = 0,
                                .expected = std::nullopt,
                                .found = std::nullopt,
                                .blocking = false,
                                .message = "C1 torn tail: log/" + segment_file_name(segments + 1)});
  CHECK(r.last_seq == 6);
  CHECK(r.checkpoint_written == 6);
  // The header was rewritten whole: the next commit lands in the new segment.
  CHECK(must(read_file(*fs, tg::segment_path(segments + 1))) ==
        bytes_t(header.begin(), header.end()));
  CHECK(rec.store->segment_count() == segments + 1);
}
