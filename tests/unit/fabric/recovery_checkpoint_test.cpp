// FAB-INV-RCV-06 (SL-ARCH-002 ARCH2-R32 and R39, `journal` checkpoint, `recovery_steps` R3 and
// R10, corruption C12): checkpoint plus tail equals the whole-log scan. The SLCK format round
// trips and refuses every malformed input with checkpoint_invalid. A store reopened after K
// commits holds a checkpoint of commit K; after D more commits, recovery from that checkpoint
// gives the same heads, last seq and open intents as recovery from segment 1, and its work is the
// D commits alone: it reads and checks exactly the frames and bytes after the checkpoint, the same
// for K = 4 and K = 12 (O(delta since the checkpoint)). Every single-bit flip of a checkpoint file,
// a checkpoint naming a commit frame the log no longer holds and a checkpoint under another name
// are ignored, reported as C12 and quarantined, and recovery then equals the whole-log scan.

#include "sl/core/error.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"

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

digest digest_of(std::string_view s) {
  digest d;
  d.bytes = sha256::hash(s);
  return d;
}

checkpoint sample() {
  checkpoint c;
  c.seq = 42;
  c.segment = 3;
  c.offset = 9000;
  c.frame_offset = 8800;
  c.last_commit = digest_of("last");
  c.refs = {ref_entry{.name = "main", .target = digest_of("main")},
            ref_entry{.name = "review", .target = digest_of("review")}};
  c.schema_set = digest_of("schemas");
  c.open_intents = {checkpoint_intent{.operation = tg::operation_named("ck/a"), .kind = "import"}};
  return c;
}

tg::harness_config config() {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.log.rotate_at = 1U << 20U;
  return cfg;
}

// A store of `before` commits, reopened (checkpoint of `before`), then `after` more commits.
struct staged_log {
  tg::crash_workload w;
  std::shared_ptr<tg::faulty_vfs> fs;
  tg::disk_image image;
};

staged_log stage(std::uint64_t seed, std::size_t before, std::size_t after,
                 const tg::harness_config &cfg) {
  staged_log out{.w = must(tg::make_workload(seed, before + after, 1, 1)),
                 .fs = std::make_shared<tg::faulty_vfs>(),
                 .image = {}};
  auto ws = must(workspace::create(tg::harness_root(), must(tg::harness_options(out.fs, cfg))));
  for (std::size_t i = 0; i < before + after; ++i) {
    if (i == before) {
      ws.reset();
      ws = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                must(tg::harness_options(out.fs, cfg))));
      REQUIRE(ws->last_recovery().checkpoint_written == before);
    }
    auto t = must(ws->manager().begin(tg::harness_tx()));
    REQUIRE(t.stage(out.w.patches[i]).has_value());
    REQUIRE(must(t.commit()).revision == out.w.heads[i + 1]);
  }
  ws.reset();
  out.image = must(tg::image_of(*out.fs->memory(), tg::harness_root()));
  return out;
}

recovery_report recover_copy(const tg::disk_image &img, const tg::harness_config &cfg,
                             bool use_checkpoint) {
  auto fs = must(tg::restore(img));
  recovery_policy p = cfg.policy;
  p.use_checkpoint = use_checkpoint;
  const recovered_log rec = must(recover_log(*fs, tg::harness_store(), p, cfg.log));
  REQUIRE_MESSAGE(rec.store, rec.report.summary());
  return rec.report;
}

void same_state(const recovery_report &a, const recovery_report &b) {
  CHECK(a.heads == b.heads);
  CHECK(a.last_seq == b.last_seq);
  CHECK(a.open_intents == b.open_intents);
  CHECK(a.lost_bytes == b.lost_bytes);
  CHECK(a.truncated_frames == b.truncated_frames);
  CHECK(a.orphan_objects == b.orphan_objects);
}

} // namespace

TEST_CASE("FAB-INV-RCV-06: the SLCK format round trips and refuses malformed files") {
  const checkpoint c = sample();
  const bytes_t bytes = must(encode_checkpoint(c));
  CHECK(bytes[0] == 'S');
  CHECK(bytes[3] == 'K');
  CHECK(bytes[4] == checkpoint_major);
  CHECK(must(decode_checkpoint(bytes)) == c);
  CHECK(checkpoint_file_name(42) == "ckpt-000000000000002a.slc");
  CHECK(parse_checkpoint_file_name("ckpt-000000000000002a.slc") == 42U);
  CHECK_FALSE(parse_checkpoint_file_name("ckpt-00000000000002a.slc").has_value());
  CHECK_FALSE(parse_checkpoint_file_name("ckpt-000000000000002A.slc").has_value());
  CHECK_FALSE(parse_checkpoint_file_name("ckpt.tmp").has_value());
  checkpoint unsorted = c;
  std::swap(unsorted.refs[0], unsorted.refs[1]);
  CHECK(encode_checkpoint(unsorted).error().code == errc::invalid_argument);
  checkpoint backwards = c;
  backwards.frame_offset = c.offset;
  CHECK(encode_checkpoint(backwards).error().code == errc::invalid_argument);
  CHECK(decode_checkpoint(bytes_t(bytes.begin(), bytes.begin() + 20)).error().code ==
        errc::checkpoint_invalid);
  bytes_t major = bytes;
  major[4] = 2;
  CHECK(decode_checkpoint(major).error().code == errc::checkpoint_invalid); // checksum first
  for (std::size_t bit = 0; bit < bytes.size() * 8; ++bit) {
    bytes_t flipped = bytes;
    flipped[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    const auto d = decode_checkpoint(flipped);
    REQUIRE_FALSE(d.has_value());
    CHECK(d.error().code == errc::checkpoint_invalid);
  }
}

TEST_CASE("FAB-INV-RCV-06: checkpoint plus tail equals the whole-log scan, at O(delta) cost") {
  const tg::harness_config cfg = config();
  for (const std::size_t before : {std::size_t{4}, std::size_t{12}}) {
    CAPTURE(before);
    const staged_log log = stage(51, before, 3, cfg);
    const bytes_t &seg = log.image.files.at(tg::segment_path(1));
    const std::vector<tg::frame_entry> frames = tg::frame_table(seg);
    std::uint64_t ckpt_end = 0;
    std::size_t commits = 0;
    for (const tg::frame_entry &f : frames) {
      if (f.type == frame_type::commit && ++commits == before) {
        ckpt_end = f.end;
      }
    }
    std::size_t delta_frames = 0;
    for (const tg::frame_entry &f : frames) {
      delta_frames += f.offset >= ckpt_end ? 1U : 0U;
    }
    const recovery_report fast = recover_copy(log.image, cfg, true);
    const recovery_report whole = recover_copy(log.image, cfg, false);
    same_state(fast, whole);
    CHECK(fast.heads.size() == 1);
    CHECK(fast.last_seq == before + 3);
    CHECK(fast.used_checkpoint);
    CHECK(fast.checkpoint_seq == before);
    CHECK_FALSE(whole.used_checkpoint);
    CHECK(fast.commits_scanned == 3);
    CHECK(fast.frames_scanned == delta_frames);
    CHECK(fast.bytes_scanned == seg.size() - ckpt_end);
    CHECK(whole.commits_scanned == before + 3);
    CHECK(whole.frames_scanned == frames.size());
    CHECK(whole.bytes_scanned == seg.size() - segment_header_size);
  }
}

TEST_CASE("FAB-INV-RCV-06: an invalid checkpoint is ignored, reported as C12 and quarantined") {
  const tg::harness_config cfg = config();
  const staged_log log = stage(52, 3, 2, cfg);
  const recovery_report whole = recover_copy(log.image, cfg, false);
  const std::string dir = (tg::harness_store() / "checkpoint").generic_string();
  const std::string name = checkpoint_file_name(3);
  const std::string path = dir + "/" + name;
  REQUIRE(log.image.files.contains(path));
  const bytes_t good = log.image.files.at(path);

  const auto expect_c12 = [&](const tg::disk_image &img, std::string_view what) -> std::string {
    auto fs = tg::restore(img);
    if (!fs) {
      return "restore";
    }
    const auto rec = recover_log(**fs, tg::harness_store(), cfg.policy, cfg.log);
    if (!rec || !rec->store) {
      return std::string(what) + ": recovery failed";
    }
    const recovery_report &r = rec->report;
    const bool ok = r.findings.size() == 1 && r.findings[0].kind == corruption_class::c12 &&
                    r.findings[0].code == errc::checkpoint_invalid && !r.findings[0].blocking &&
                    !r.used_checkpoint && r.heads == whole.heads && r.last_seq == whole.last_seq &&
                    r.open_intents == whole.open_intents && r.quarantined.size() == 1 &&
                    r.checkpoint_written == whole.last_seq;
    return ok ? std::string() : std::string(what) + ": " + r.summary();
  };

  std::vector<std::string> out(good.size() * 8);
  tg::parallel_for(out.size(), [&](std::size_t bit) {
    tg::disk_image img = log.image;
    img.files[path][bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    out[bit] = expect_c12(img, "bit " + std::to_string(bit));
  });
  std::size_t failed = 0;
  for (const std::string &o : out) {
    if (!o.empty() && ++failed <= 10) {
      CHECK_MESSAGE(o.empty(), o);
    }
  }
  CHECK(failed == 0);

  // A checkpoint whose commit frame the log no longer holds (the tail was lost after it).
  {
    tg::disk_image img = log.image;
    checkpoint c = must(decode_checkpoint(good));
    c.seq = 9;
    c.frame_offset += 100000;
    c.offset += 100000;
    img.files.erase(path);
    img.files[dir + "/" + checkpoint_file_name(9)] = must(encode_checkpoint(c));
    CHECK(expect_c12(img, "beyond the log").empty());
  }
  // A valid checkpoint under another seq's name.
  {
    tg::disk_image img = log.image;
    img.files.erase(path);
    img.files[dir + "/" + checkpoint_file_name(4)] = good;
    CHECK(expect_c12(img, "renamed").empty());
  }
  // After the repair the next recovery uses the new checkpoint and finds nothing.
  {
    tg::disk_image img = log.image;
    img.files[path][40] ^= 0x01U;
    auto fs = must(tg::restore(img));
    REQUIRE(must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log)).store);
    const recovered_log again = must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log));
    CHECK(again.report.lossless());
    CHECK(again.report.used_checkpoint);
    CHECK(again.report.checkpoint_seq == whole.last_seq);
  }
}
