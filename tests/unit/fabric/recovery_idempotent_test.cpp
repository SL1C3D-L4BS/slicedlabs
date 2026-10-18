// FAB-INV-RCV-04 (SL-ARCH-002 ARCH2-R35, `recovery_steps` R8 and R10): recovery is idempotent. A
// store is left dirty in every way recovery repairs at once: a stale lock holder text, an invalid
// checkpoint, a loose blob .tmp of a commit that never landed, an orphan object frame and a torn
// frame after the last commit. The first write open repairs it (C11, C12 and C1 findings, the tail,
// the .tmp and the checkpoint quarantined, a new checkpoint written); a second open reports zero
// loss and the same heads, and changes no byte on disk; an inspect run then reports nothing. An
// inspect run before the repair reports exactly what the repair will do and changes nothing.

#include "sl/core/error.hpp"
#include "sl/fabric/checkpoint.hpp"
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

// The workload's store with every kind of dirt recovery repairs.
tg::disk_image dirty_store(const tg::built_log &log) {
  tg::disk_image img = log.image;
  const std::string store = tg::harness_store().generic_string();
  img.files[store + "/lock"] = text("slfabric writer pid 4242 since 1\n");
  img.dirs.insert(store + "/checkpoint");
  img.files[store + "/checkpoint/" + checkpoint_file_name(99)] = text("not a checkpoint");
  const object blob = make_object(object_kind::blob, text("a blob that never landed"));
  const std::string hex = blob.id.to_hex();
  img.dirs.insert(store + "/loose/" + hex.substr(0, 2));
  img.files[store + "/loose/" + hex.substr(0, 2) + "/" + hex + ".tmp"] =
      frame_object(blob.header, blob.payload);
  bytes_t &seg = img.files[tg::segment_path(1)];
  const object orphan = make_object(object_kind::blob, text("an orphan object frame"));
  const bytes_t frame =
      encode_frame(frame_type::object, frame_object(orphan.header, orphan.payload))
          .value_or(bytes_t{});
  seg.insert(seg.end(), frame.begin(), frame.end());
  const bytes_t torn = encode_frame(frame_type::object, text("torn")).value_or(bytes_t{});
  seg.insert(seg.end(), torn.begin(), torn.begin() + 7);
  return img;
}

} // namespace

TEST_CASE("FAB-INV-RCV-04: recovery is idempotent") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  cfg.log.rotate_at = 1U << 20U;
  const tg::built_log log = must(tg::build_log(31, 4, cfg));
  const tg::disk_image dirty = dirty_store(log);
  auto fs = must(tg::restore(dirty));

  // Inspect first: the same verdict, nothing touched, the store not opened.
  const recovered_log looked =
      must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log, recovery_mode::inspect));
  CHECK_FALSE(looked.store);
  CHECK(looked.report.quarantined.empty());
  CHECK(looked.report.orphan_objects == 1);
  CHECK(must(tg::image_of(*fs->memory(), tg::harness_root())) == dirty);

  auto first = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                    must(tg::harness_options(fs, cfg))));
  const recovery_report r1 = first->last_recovery();
  REQUIRE(r1.findings.size() == 3);
  CHECK(r1.findings[0].kind == corruption_class::c11);
  CHECK(r1.findings[1].kind == corruption_class::c12);
  CHECK(r1.findings[2].kind == corruption_class::c1);
  CHECK(r1.stale_lock == "slfabric writer pid 4242 since 1");
  CHECK(r1.orphan_objects == looked.report.orphan_objects);
  CHECK(r1.lost_bytes == looked.report.lost_bytes);
  CHECK(r1.truncated_frames == 2);
  CHECK(r1.quarantined.size() == 3);
  CHECK(r1.checkpoint_written == 4);
  CHECK(r1.last_seq == 4);
  const auto heads = r1.heads;
  CHECK(must(first->manager().head()).revision_digest() == log.workload.heads[4]);
  first.reset();
  const tg::disk_image after_first = must(tg::image_of(*fs->memory(), tg::harness_root()));

  auto second = must(workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                     must(tg::harness_options(fs, cfg))));
  const recovery_report &r2 = second->last_recovery();
  CHECK_MESSAGE(r2.lossless(), r2.summary());
  CHECK(r2.heads == heads);
  CHECK(r2.last_seq == 4);
  CHECK(r2.used_checkpoint);
  CHECK(r2.checkpoint_seq == 4);
  CHECK(r2.checkpoint_written == 0);
  CHECK(r2.frames_scanned == 0);
  second.reset();
  CHECK(must(tg::image_of(*fs->memory(), tg::harness_root())) == after_first);

  const recovered_log again =
      must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log, recovery_mode::inspect));
  CHECK(again.report.lossless());
  CHECK(again.report.heads == heads);
  CHECK(again.store);
}

TEST_CASE("FAB-INV-RCV-04: recovering a recovered store from the whole log changes nothing") {
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  const tg::built_log log = must(tg::build_log(32, 3, cfg));
  auto fs = must(tg::restore(dirty_store(log)));
  const recovered_log first = must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log));
  REQUIRE_FALSE(first.report.blocking());
  const tg::disk_image repaired = must(tg::image_of(*fs->memory(), tg::harness_root()));
  recovery_policy whole = cfg.policy;
  whole.use_checkpoint = false;
  whole.write_checkpoint = false;
  for (int round = 0; round < 3; ++round) {
    CAPTURE(round);
    const recovered_log again = must(recover_log(*fs, tg::harness_store(), whole, cfg.log));
    CHECK_MESSAGE(again.report.lossless(), again.report.summary());
    CHECK(again.report.heads == first.report.heads);
    CHECK(again.report.last_seq == first.report.last_seq);
    CHECK(must(tg::image_of(*fs->memory(), tg::harness_root())) == repaired);
  }
}
