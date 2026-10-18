// FAB-INV-COR-01 (SL-ARCH-002 ARCH2-R41, ARCH2-R43, `corruption` C1-C12): every corruption class
// planted into a sample store is detected by fsck with its evidence (class, file, offset, object
// or record id) and handled by its policy. For each class: fsck names it; a strict open refuses
// (format_unsupported for C8, store_corrupt otherwise) and leaves every byte as it was; a lenient
// open refuses C8, opens a read-only recovered view for C2-C7 and C10 (ref recovered/<seq>,
// commits refused, damaged bytes quarantined where the policy quarantines, C7 and C10 records
// flagged) and opens the head for write for C1, C9, C11 and C12 (C9 records flagged).

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/corrupt_store.hpp"
#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
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

fsck_report run_fsck(const tg::sample_store &s, const tg::sample_config &cfg) {
  const workspace_options o = must(tg::sample_options(s.fs, cfg));
  return must(fsck(*s.fs, s.root, fsck_options{.schemas = &o.schemas}));
}

std::vector<finding> of_class(const fsck_report &r, corruption_class c) {
  std::vector<finding> out;
  std::ranges::copy_if(r.findings, std::back_inserter(out),
                       [&](const finding &f) { return f.cls == c; });
  return out;
}

bool names_oid(const fsck_report &r, const digest &oid) {
  return std::ranges::any_of(r.findings, [&](const finding &f) {
    return f.where.oid == oid && f.line().contains(oid.to_hex());
  });
}

void dump(const fsck_report &r) {
  for (const finding &f : r.findings) {
    MESSAGE(f.line());
  }
}

// The policy of `c` at open (file comment).
void check_policy(const tg::sample_store &s, const tg::sample_config &cfg, corruption_class c,
                  const std::optional<record_id> &flag = std::nullopt) {
  const corruption_policy &p = policy_of(c);
  CAPTURE(p.name);
  const tg::disk_image before = must(tg::image_of(*s.fs, s.root));
  const auto strict = open_checked(s.root, corruption_mode::strict, recovery_policy{},
                                   must(tg::sample_options(s.fs, cfg)));
  REQUIRE_FALSE(strict.has_value());
  CHECK(strict.error().code == (p.refuses ? errc::format_unsupported : errc::store_corrupt));
  CHECK(must(tg::image_of(*s.fs, s.root)) == before);

  auto lenient = open_checked(s.root, corruption_mode::lenient, recovery_policy{},
                              must(tg::sample_options(s.fs, cfg)));
  if (p.refuses) {
    REQUIRE_FALSE(lenient.has_value());
    CHECK(lenient.error().code == errc::format_unsupported);
    CHECK(must(tg::image_of(*s.fs, s.root)) == before);
    return;
  }
  REQUIRE_MESSAGE(lenient.has_value(), (lenient ? std::string() : lenient.error().message));
  CHECK(std::ranges::any_of(lenient->findings, [&](const finding &f) { return f.cls == c; }));
  if (flag) {
    CHECK(std::ranges::binary_search(lenient->flagged, *flag));
  }
  workspace &ws = *lenient->ws;
  if (p.read_only_view) {
    CHECK(lenient->recovered_view);
    CHECK(lenient->recovered_ref.starts_with("recovered/"));
    REQUIRE(lenient->recovered_revision.has_value());
    const digest rev = lenient->recovered_revision.value_or(digest{});
    CHECK(must(ws.store().read_ref(lenient->recovered_ref)) == rev);
    CHECK(must(ws.store().list_refs()).size() == 1);
    CHECK(must(state_closure_verifies(ws.store(), rev)));
    // C5: the object is absent, so there are no bytes to quarantine.
    if (c != corruption_class::c5) {
      CHECK(p.quarantines == !lenient->quarantined.empty());
    }
    for (const std::string &q : lenient->quarantined) {
      CHECK(q.starts_with("quarantine/"));
    }
    const auto refused =
        tg::commit_ops(ws.manager(), tg::tx_origin_of(tg::tx_editor()),
                       {must(tg::create_entity("after"))}, {}, lenient->recovered_ref);
    CHECK_FALSE(refused.has_value());
  } else {
    CHECK_FALSE(lenient->recovered_view);
    CHECK(ws.mode() == open_mode::write);
    CHECK(must(tg::commit_ops(ws.manager(), tg::tx_origin_of(tg::tx_editor()),
                              {must(tg::create_entity("after"))}))
              .seq > 0);
  }
}

record_object e1_record(const tg::sample_store &s, const tg::sample_config &cfg) {
  auto ws = must(tg::open_sample(s, cfg));
  return must(tg::record_at(ws->store(), s.heads.back(), tg::entity_named("e1")));
}

} // namespace

TEST_CASE("FAB-INV-COR-01: the sample store is clean and opens in both modes") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  CHECK(r.clean());
  CHECK(r.records_unjudged == 0);
  CHECK(r.records_checked > 0);
  CHECK(r.heads.size() == 1);
  const checked_open strict = must(open_checked(s.root, corruption_mode::strict, recovery_policy{},
                                                must(tg::sample_options(s.fs, cfg))));
  CHECK_FALSE(strict.recovered_view);
  CHECK(strict.findings.empty());
}

TEST_CASE("FAB-INV-COR-01: C1 torn tail is found at the last segment and self-heals") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_truncate(*s.fs, s.dir, 5));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c1 = of_class(r, corruption_class::c1);
  REQUIRE(c1.size() == 1);
  CHECK(r.findings.size() == 1);
  CHECK(c1[0].where.file == p.file);
  CHECK(c1[0].where.offset < p.offset);
  CHECK(c1[0].line().contains("offset"));
  check_policy(s, cfg, corruption_class::c1);
}

TEST_CASE("FAB-INV-COR-01: C2 mid-log damage names the frame offset, the intact commits after "
          "it and the lost object") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_flip_object(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c2 = of_class(r, corruption_class::c2);
  REQUIRE(c2.size() == 1);
  CHECK(c2[0].where.file == p.file);
  CHECK(c2[0].where.offset == p.offset);
  CHECK(c2[0].commits_after >= 1);
  REQUIRE(p.oid.has_value());
  CHECK(names_oid(r, p.oid.value_or(digest{})));
  check_policy(s, cfg, corruption_class::c2);
}

TEST_CASE("FAB-INV-COR-01: C3 a deleted middle commit breaks the chain") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_delete_commit(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c3 = of_class(r, corruption_class::c3);
  REQUIRE(c3.size() == 1);
  CHECK(c3[0].where.file == p.file);
  CHECK(c3[0].where.offset >= p.offset); // the commit frame after the cut
  CHECK(c3[0].detail.contains("expected seq " + std::to_string(p.seq)));
  check_policy(s, cfg, corruption_class::c3);
}

TEST_CASE("FAB-INV-COR-01: C4 a loose blob whose bytes do not hash to its name") {
  tg::sample_config cfg;
  cfg.loose_above = 64;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_flip_loose(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c4 = of_class(r, corruption_class::c4);
  REQUIRE_FALSE(c4.empty());
  CHECK(c4[0].where.file == p.file);
  CHECK(c4[0].where.oid == p.oid);
  check_policy(s, cfg, corruption_class::c4);
}

TEST_CASE("FAB-INV-COR-01: C5 a removed loose blob is a missing object") {
  tg::sample_config cfg;
  cfg.loose_above = 64;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_remove_loose(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c5 = of_class(r, corruption_class::c5);
  REQUIRE_FALSE(c5.empty());
  CHECK(c5[0].where.oid == p.oid);
  REQUIRE(p.oid.has_value());
  CHECK(c5[0].line().contains(p.oid.value_or(digest{}).to_hex()));
  check_policy(s, cfg, corruption_class::c5);
}

TEST_CASE("FAB-INV-COR-01: C6 a record object that does not decode") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const std::vector<std::uint8_t> garbage = {0xff, 0x01, 0x02, 0x03};
  const object bad = make_object(object_kind::record, garbage);
  (void)must(tg::plant_record(s, tg::entity_named("e1"), bad, cfg));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c6 = of_class(r, corruption_class::c6);
  REQUIRE(c6.size() == 1);
  CHECK(c6[0].where.oid == bad.id);
  check_policy(s, cfg, corruption_class::c6);
}

TEST_CASE("FAB-INV-COR-01: C7 a record body that fails its schema is flagged") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  record_object rec = e1_record(s, cfg);
  rec.body = value::uint(7);
  const object bad = must(encode_record_object(rec));
  (void)must(tg::plant_record(s, rec.id, bad, cfg));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c7 = of_class(r, corruption_class::c7);
  REQUIRE(c7.size() == 1);
  CHECK(c7[0].where.record == rec.id);
  CHECK(c7[0].where.oid == bad.id);
  CHECK(c7[0].code == errc::schema_invalid);
  check_policy(s, cfg, corruption_class::c7, rec.id);
}

TEST_CASE("FAB-INV-COR-01: C8 a newer format is refused and nothing is touched") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  (void)must(tg::plant_newer_format(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  REQUIRE(r.findings.size() == 1);
  CHECK(r.findings[0].cls == corruption_class::c8);
  CHECK(r.findings[0].code == errc::format_unsupported);
  CHECK(r.findings[0].where.file == "FORMAT");
  CHECK(r.findings[0].detail.contains("supported 1.0"));
  check_policy(s, cfg, corruption_class::c8);
}

TEST_CASE("FAB-INV-COR-01: C9 a strong reference to a record that is not live") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const record_id ghost = tg::asset_named("ghost");
  const patch_op op = must(tg::create_cooked("c9", ghost));
  const workspace_options o = must(tg::sample_options(s.fs, cfg));
  const type_desc *t = o.schemas.find(op.type);
  REQUIRE(t != nullptr);
  const value body = op.new_value.value_or(value{});
  const object cooked = must(encode_record_object(record_object{.id = op.target,
                                                                .type = op.type,
                                                                .document = op.target,
                                                                .version = t->version,
                                                                .body = body}));
  (void)must(tg::plant_record(s, op.target, cooked, cfg));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c9 = of_class(r, corruption_class::c9);
  REQUIRE(c9.size() == 1);
  CHECK(r.findings.size() == 1);
  CHECK(c9[0].where.record == op.target);
  CHECK(c9[0].target == ghost);
  CHECK(c9[0].detail.contains("at path"));
  check_policy(s, cfg, corruption_class::c9, op.target);
}

TEST_CASE("FAB-INV-COR-01: C10 a record whose id class is not its type's record class") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  record_object rec = e1_record(s, cfg);
  rec.id = tg::asset_named("c10");
  const object bad = must(encode_record_object(rec));
  (void)must(tg::plant_record(s, rec.id, bad, cfg));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c10 = of_class(r, corruption_class::c10);
  REQUIRE(c10.size() == 1);
  CHECK(c10[0].where.record == rec.id);
  CHECK(c10[0].code == errc::id_class_mismatch);
  check_policy(s, cfg, corruption_class::c10, rec.id);
}

TEST_CASE("FAB-INV-COR-01: C11 a stale lock is found by the probe and taken over") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  (void)must(tg::plant_stale_lock(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  REQUIRE(r.findings.size() == 1);
  CHECK(r.findings[0].cls == corruption_class::c11);
  CHECK(r.findings[0].where.file == "lock");
  CHECK(r.findings[0].detail.contains("pid 4242"));
  check_policy(s, cfg, corruption_class::c11);
}

TEST_CASE("FAB-INV-COR-01: C12 a broken checkpoint is found and recovery rescans") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const auto made = tg::make_checkpoint(s, cfg);
  REQUIRE_MESSAGE(made.has_value(), (made ? std::string() : made.error().message));
  const tg::planted p = must(tg::plant_broken_checkpoint(*s.fs, s.dir));
  const fsck_report r = run_fsck(s, cfg);
  dump(r);
  const auto c12 = of_class(r, corruption_class::c12);
  REQUIRE(c12.size() == 1);
  CHECK(c12[0].where.file == p.file);
  CHECK(c12[0].code == errc::checkpoint_invalid);
  check_policy(s, cfg, corruption_class::c12);
}
