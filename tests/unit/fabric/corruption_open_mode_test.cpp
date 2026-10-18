// FAB-INV-COR-05 (SL-ARCH-002 ARCH2-R43): strict open refuses with store_corrupt; lenient open is
// read-only at the newest verifiable revision. A clean store opens for write in both modes. With
// a torn tail, a schema-invalid record or a stale lock a strict open refuses with store_corrupt
// naming the finding and leaves every byte as it was; a newer format is refused in both modes. A
// lenient open of a store with a commit cut out of the log exposes only ref recovered/<seq> at
// the head before the cut and refuses commits; one whose newest head holds an undecodable record
// opens at the head before it (the newest whose closure verifies). A live writer refuses both.

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/corrupt_store.hpp"
#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
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

sl::result<checked_open> open_in(const tg::sample_store &s, const tg::sample_config &cfg,
                                 corruption_mode mode) {
  return open_checked(s.root, mode, recovery_policy{}, must(tg::sample_options(s.fs, cfg)));
}

std::size_t index_of_seq(const tg::sample_store &s, std::uint64_t seq) {
  const auto it = std::ranges::find(s.seqs, seq);
  REQUIRE(it != s.seqs.end());
  return static_cast<std::size_t>(it - s.seqs.begin());
}

} // namespace

TEST_CASE("FAB-INV-COR-05: a clean store opens for write in both modes") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  for (const corruption_mode mode : {corruption_mode::strict, corruption_mode::lenient}) {
    checked_open o = must(open_in(s, cfg, mode));
    CHECK_FALSE(o.recovered_view);
    CHECK(o.findings.empty());
    CHECK(o.ws->mode() == open_mode::write);
    CHECK(must(o.ws->store().read_ref("main")) == s.heads.back());
  }
}

TEST_CASE("FAB-INV-COR-05: strict refuses any finding with store_corrupt and touches nothing") {
  const tg::sample_config cfg;
  const std::vector<std::pair<std::string, std::function<void(const tg::sample_store &)>>> plants =
      {
          {"C1",
           [](const tg::sample_store &s) { (void)must(tg::plant_truncate(*s.fs, s.dir, 4)); }},
          {"C7",
           [&](const tg::sample_store &s) {
             record_object r;
             {
               auto ws = must(tg::open_sample(s, cfg));
               r = must(tg::record_at(ws->store(), s.heads.back(), tg::entity_named("e2")));
             }
             r.body = value::uint(3);
             (void)must(tg::plant_record(s, r.id, must(encode_record_object(r)), cfg));
           }},
          {"C11",
           [](const tg::sample_store &s) { (void)must(tg::plant_stale_lock(*s.fs, s.dir)); }},
  };
  for (const auto &[cls, plant] : plants) {
    CAPTURE(cls);
    const tg::sample_store s = must(tg::make_sample_store(cfg));
    plant(s);
    const tg::disk_image before = must(tg::image_of(*s.fs, s.root));
    const auto strict = open_in(s, cfg, corruption_mode::strict);
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().code == errc::store_corrupt);
    CHECK(strict.error().message.contains(cls + " "));
    CHECK(must(tg::image_of(*s.fs, s.root)) == before);
  }
}

TEST_CASE("FAB-INV-COR-05: a newer format is refused in both modes") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  (void)must(tg::plant_newer_format(*s.fs, s.dir, "slfabric 3.1\n"));
  const tg::disk_image before = must(tg::image_of(*s.fs, s.root));
  for (const corruption_mode mode : {corruption_mode::strict, corruption_mode::lenient}) {
    const auto o = open_in(s, cfg, mode);
    REQUIRE_FALSE(o.has_value());
    CHECK(o.error().code == errc::format_unsupported);
  }
  CHECK(must(tg::image_of(*s.fs, s.root)) == before);
}

TEST_CASE("FAB-INV-COR-05: lenient is a read-only view at recovered/<seq> before a cut commit") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const tg::planted p = must(tg::plant_delete_commit(*s.fs, s.dir));
  REQUIRE_FALSE(open_in(s, cfg, corruption_mode::strict).has_value());
  checked_open view = must(open_in(s, cfg, corruption_mode::lenient));
  REQUIRE(view.recovered_view);
  const std::uint64_t seq = p.seq - 1;
  CHECK(view.recovered_ref == "recovered/" + std::to_string(seq));
  REQUIRE(view.recovered_revision.has_value());
  const digest rev = view.recovered_revision.value_or(digest{});
  CHECK(rev == s.heads[index_of_seq(s, seq)]);
  const std::vector<ref_entry> refs = must(view.ws->store().list_refs());
  REQUIRE(refs.size() == 1);
  CHECK(refs[0].name == view.recovered_ref);
  CHECK(must(view.ws->manager().head(view.recovered_ref)).root_digest() ==
        must(snapshot::open(view.ws->store(), rev)).root_digest());

  const auto refused = tg::commit_ops(view.ws->manager(), tg::tx_origin_of(tg::tx_editor()),
                                      {must(tg::create_entity("late"))}, {}, view.recovered_ref);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::permission_denied);
  CHECK(must(view.ws->store().read_ref(view.recovered_ref)) == rev);
}

TEST_CASE("FAB-INV-COR-05: lenient opens the newest revision whose closure verifies") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const std::vector<std::uint8_t> garbage = {0xfe, 0x01};
  const digest bad = must(
      tg::plant_record(s, tg::entity_named("e3"), make_object(object_kind::record, garbage), cfg));
  checked_open view = must(open_in(s, cfg, corruption_mode::lenient));
  REQUIRE(view.recovered_view);
  CHECK(view.recovered_revision == s.heads.back());
  CHECK(view.recovered_revision != bad);
  CHECK(view.recovered_ref == "recovered/" + std::to_string(s.seqs.back()));
  CHECK(must(state_closure_verifies(view.ws->store(), s.heads.back())));
}

TEST_CASE("FAB-INV-COR-05: a live writer refuses both modes") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  auto writer = must(tg::open_sample(s, cfg));
  for (const corruption_mode mode : {corruption_mode::strict, corruption_mode::lenient}) {
    const auto o = open_in(s, cfg, mode);
    REQUIRE_FALSE(o.has_value());
    CHECK(o.error().code == errc::writer_locked);
  }
}
