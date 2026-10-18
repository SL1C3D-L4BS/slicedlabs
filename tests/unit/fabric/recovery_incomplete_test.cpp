// FAB-INV-RCV-05 (SL-ARCH-002 ARCH2-R36 and R37, `recovery_steps` R6 and R7): open intents and
// orphan objects are reported and never visible. An intent_begin committed without its
// intent_end is an open intent: reported with its kind and the options continue or compensate,
// kept across reopens (from the checkpoint and from the whole log) and never closed by recovery.
// Object and intent frames after the last commit are orphans: counted, cut, never readable from
// the store, and the intents among them are not reported as open. Inspect mode reports the same
// counts without touching the log, and a read-only open of such a log is recovery_required.

#include "sl/core/error.hpp"
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

intent_record begin(const record_id &op, std::string kind) {
  return intent_record{.operation = op,
                       .kind = std::move(kind),
                       .phase = intent_phase::begin,
                       .outcome = intent_outcome::none};
}

intent_record end(const record_id &op, intent_outcome outcome) {
  return intent_record{.operation = op, .kind = {}, .phase = intent_phase::end, .outcome = outcome};
}

tg::harness_config config() {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.log.rotate_at = 1U << 20U;
  return cfg;
}

} // namespace

TEST_CASE("FAB-INV-RCV-05: an open intent is reported with its options and kept across reopens") {
  const tg::harness_config cfg = config();
  const tg::built_log log = must(tg::build_log(41, 3, cfg));
  const record_id x = tg::operation_named("incomplete/x");
  const record_id y = tg::operation_named("incomplete/y");
  REQUIRE(must(tg::commit_intents(*log.fs, cfg.log,
                                  {begin(x, "import"), begin(y, "compensate"),
                                   end(y, intent_outcome::compensated)})) == 4);
  for (const bool checkpoint : {false, true, true}) {
    CAPTURE(checkpoint);
    recovery_policy p = cfg.policy;
    p.use_checkpoint = checkpoint;
    auto ws = must(workspace::open(tg::harness_root(), open_mode::write, p,
                                   must(tg::harness_options(log.fs, cfg))));
    const recovery_report &r = ws->last_recovery();
    REQUIRE(r.open_intents.size() == 1);
    CHECK(r.open_intents[0].operation == x);
    CHECK(r.open_intents[0].kind == "import");
    CHECK(r.open_intents[0].options == std::vector<std::string>{"continue", "compensate"});
    CHECK(r.findings.empty());
    CHECK(r.last_seq == 4);
    CHECK(must(ws->manager().head()).revision_digest() == log.workload.heads[3]);
  }
  // Recovery never closes an intent (no roll-forward, ARCH2-R37): it is still open in the store.
  auto store = must(log_backend::open(*log.fs, tg::harness_store(), cfg.log));
  CHECK(store->open_intents() == std::vector<record_id>{x});
}

TEST_CASE("FAB-INV-RCV-05: orphan object and intent frames after the last commit are counted, "
          "cut and never visible") {
  const tg::harness_config cfg = config();
  const tg::built_log log = must(tg::build_log(42, 3, cfg));
  tg::disk_image img = log.image;
  bytes_t &seg = img.files[tg::segment_path(1)];
  const std::uint64_t clean_size = seg.size();
  std::vector<object> orphans;
  for (const std::string_view body : {"orphan one", "orphan two"}) {
    orphans.push_back(make_object(object_kind::blob, text(body)));
    const bytes_t f = must(encode_frame(
        frame_type::object, frame_object(orphans.back().header, orphans.back().payload)));
    seg.insert(seg.end(), f.begin(), f.end());
  }
  const record_id z = tg::operation_named("incomplete/z");
  const bytes_t intent =
      must(encode_frame(frame_type::intent_begin, must(encode_intent(begin(z, "import")))));
  seg.insert(seg.end(), intent.begin(), intent.end());
  const std::uint64_t dirty_size = seg.size();

  SUBCASE("inspect reports and touches nothing; a read-only open needs the repair") {
    auto fs = must(tg::restore(img));
    const recovered_log rec =
        must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log, recovery_mode::inspect));
    CHECK_FALSE(rec.store);
    CHECK(rec.report.orphan_objects == 2);
    CHECK(rec.report.truncated_frames == 3);
    CHECK(rec.report.lost_bytes == dirty_size - clean_size);
    CHECK(rec.report.open_intents.empty());
    CHECK(must(tg::image_of(*fs->memory(), tg::harness_root())) == img);
    const auto ro = workspace::open(tg::harness_root(), open_mode::read_only, cfg.policy,
                                    must(tg::harness_options(fs, cfg)));
    REQUIRE_FALSE(ro.has_value());
    CHECK(ro.error().code == errc::recovery_required);
  }
  SUBCASE("repair cuts them; the store never serves them") {
    auto fs = must(tg::restore(img));
    auto ws = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                   must(tg::harness_options(fs, cfg))));
    const recovery_report &r = ws->last_recovery();
    CHECK(r.orphan_objects == 2);
    CHECK(r.truncated_frames == 3);
    CHECK(r.lost_bytes == dirty_size - clean_size);
    CHECK(r.findings.empty()); // whole frames: orphans, not a torn tail
    CHECK(r.open_intents.empty());
    REQUIRE(r.quarantined.size() == 1);
    for (const object &o : orphans) {
      CHECK_FALSE(must(ws->store().has(o.id)));
      CHECK(ws->store().get(o.id).error().code == errc::object_missing);
    }
    CHECK(must(read_file(*fs, tg::segment_path(1))).size() == clean_size);
    CHECK(must(ws->manager().head()).revision_digest() == log.workload.heads[3]);
  }
}
