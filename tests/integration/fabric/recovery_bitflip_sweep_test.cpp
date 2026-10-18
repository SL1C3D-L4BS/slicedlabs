// FAB-INV-RCV-02 (SL-ARCH-002 ARCH2-R30, `recovery_steps` R4, corruption C1 and C2): every
// single-bit flip in committed frames is detected or provably harmless. The small log holds three
// workspace commits (scaled by SL_FABRIC_SWEEP_SCALE, at least one) and a last commit of intent
// frames only (an open intent). Every bit of the segment file is flipped in turn and recovery
// runs from segment 1. CRC-32C covers the type and payload of every frame and the segment header
// carries its own CRC, so no flip is harmless: a flip in the header or in any frame but the last is
// mid-log damage, a blocking C2 finding at the damaged frame's offset (offset 0 for the header)
// whose evidence counts the intact commits after it, and recovery touches nothing; a flip in the
// last frame leaves no valid frame after it, so it is a torn tail (C1): the last commit is cut
// whole, its bytes are quarantined, and the store recovers to the workspace commits with no open
// intent. The cases run on worker threads.

#include "sl/core/error.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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

std::optional<digest> main_head(const recovery_report &r) {
  for (const ref_entry &e : r.heads) {
    if (e.name == "main") {
      return e.target;
    }
  }
  return std::nullopt;
}

struct flip_outcome {
  std::string error;
  bool torn = false;
};

} // namespace

TEST_CASE("FAB-INV-RCV-02: every single-bit flip in committed frames is detected or provably "
          "harmless") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.policy.open_store = false;
  const std::size_t n = tg::scaled(3, 1); // workspace commits before the intents commit
  const tg::crash_workload w = must(tg::make_workload(5, n, 1, 1));
  auto built = std::make_shared<tg::faulty_vfs>();
  REQUIRE(!tg::run_workload(built, w, cfg, {}).failure);
  const record_id x = tg::operation_named("flip/x");
  const record_id y = tg::operation_named("flip/y");
  REQUIRE(must(tg::commit_intents(*built, cfg.log,
                                  {intent_record{.operation = x,
                                                 .kind = "compensate",
                                                 .phase = intent_phase::begin,
                                                 .outcome = intent_outcome::none},
                                   intent_record{.operation = y,
                                                 .kind = "import",
                                                 .phase = intent_phase::begin,
                                                 .outcome = intent_outcome::none},
                                   intent_record{.operation = y,
                                                 .kind = {},
                                                 .phase = intent_phase::end,
                                                 .outcome = intent_outcome::completed}})) == n + 1);
  const tg::disk_image image = must(tg::image_of(*built->memory(), tg::harness_root()));
  const std::string seg = (tg::harness_store() / "log" / segment_file_name(1)).generic_string();
  REQUIRE(image.files.contains(seg));
  REQUIRE_FALSE(
      image.files.contains((tg::harness_store() / "log" / segment_file_name(2)).generic_string()));
  const std::vector<std::uint8_t> &bytes = image.files.at(seg);
  const std::vector<tg::frame_entry> frames = tg::frame_table(bytes);
  REQUIRE(frames.size() >= 5);
  REQUIRE(frames.back().type == frame_type::commit);
  REQUIRE(frames.back().end == bytes.size());
  // The last commit: three intent frames and its commit frame.
  const std::uint64_t last_group = frames[frames.size() - 4].offset;

  // The unflipped log recovers losslessly with the open intent.
  {
    auto fs = must(tg::restore_memory(image));
    const recovered_log rec = must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log));
    CHECK(rec.report.lossless());
    CHECK(rec.report.last_seq == n + 1);
    REQUIRE(rec.report.open_intents.size() == 1);
    CHECK(rec.report.open_intents[0].operation == x);
    CHECK(rec.report.open_intents[0].kind == "compensate");
    CHECK(rec.report.open_intents[0].options == std::vector<std::string>{"continue", "compensate"});
  }

  const std::size_t bits = bytes.size() * 8;
  std::vector<flip_outcome> out(bits);
  tg::parallel_for(bits, [&](std::size_t bit) {
    flip_outcome &o = out[bit];
    const std::size_t at = bit / 8;
    tg::disk_image flipped = image;
    flipped.files[seg][at] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    auto fs = tg::restore_memory(flipped);
    if (!fs) {
      o.error = "restore: " + fs.error().message;
      return;
    }
    const auto rec = recover_log(**fs, tg::harness_store(), cfg.policy, cfg.log);
    if (!rec) {
      o.error = "recover_log: " + rec.error().message;
      return;
    }
    const recovery_report &r = rec->report;
    const auto frame = std::ranges::find_if(
        frames, [at](const tg::frame_entry &f) { return at >= f.offset && at < f.end; });
    const bool last = frame != frames.end() && frame + 1 == frames.end();
    if (last) {
      o.torn = true;
      const bool ok = !r.blocking() && r.findings.size() == 1 &&
                      r.findings[0].kind == corruption_class::c1 &&
                      r.findings[0].offset == frame->offset && r.findings[0].seq == n &&
                      r.lost_bytes == bytes.size() - last_group && r.truncated_frames == 4 &&
                      r.orphan_objects == 0 && r.last_seq == n && r.open_intents.empty() &&
                      main_head(r) == w.heads[n] && r.quarantined.size() == 1;
      if (!ok) {
        o.error = "expected a C1 cut of the last commit: " + r.summary();
      }
      return;
    }
    const std::uint64_t offset = frame == frames.end() ? 0 : frame->offset;
    const auto after = static_cast<std::uint64_t>(
        std::ranges::count_if(frames, [offset](const tg::frame_entry &f) {
          return f.type == frame_type::commit && f.offset > offset;
        }));
    const bool ok = r.blocking() && !rec->store && r.findings.size() == 1 &&
                    r.findings[0].kind == corruption_class::c2 &&
                    r.findings[0].code == errc::journal_corrupt && r.findings[0].offset == offset &&
                    r.findings[0].commits_after == after &&
                    r.findings[0].file == "log/" + segment_file_name(1) && r.quarantined.empty() &&
                    r.lost_bytes == 0;
    if (!ok) {
      o.error = "expected C2 at offset " + std::to_string(offset) + " with " +
                std::to_string(after) + " commits after: " + r.summary();
      return;
    }
    // Nothing was touched.
    const auto now = read_file(**fs, seg);
    if (!now || *now != flipped.files[seg]) {
      o.error = "a blocking finding changed the segment";
    }
  });

  std::size_t failed = 0;
  std::size_t torn = 0;
  for (std::size_t bit = 0; bit < bits; ++bit) {
    torn += out[bit].torn ? 1U : 0U;
    if (!out[bit].error.empty() && ++failed <= 20) {
      CAPTURE(bit);
      CHECK_MESSAGE(out[bit].error.empty(), out[bit].error);
    }
  }
  CHECK(failed == 0);
  CHECK(torn == (frames.back().end - frames.back().offset) * 8);
  MESSAGE("bit flip sweep: " << bits << " flips over " << bytes.size() << " bytes, "
                             << frames.size() << " frames: C2 " << bits - torn << ", C1 " << torn
                             << ", harmless 0, failed " << failed);

  // A flipped segment header refuses the write open with journal_corrupt.
  tg::disk_image header = image;
  header.files[seg][13] ^= 0x10U;
  auto fs = must(tg::restore(header));
  const auto ws = workspace::open(tg::harness_root(), open_mode::write, recovery_policy{},
                                  must(tg::harness_options(fs, cfg)));
  REQUIRE_FALSE(ws.has_value());
  CHECK(ws.error().code == errc::journal_corrupt);
  CHECK(ws.error().message.find("C2") != std::string::npos);
}
