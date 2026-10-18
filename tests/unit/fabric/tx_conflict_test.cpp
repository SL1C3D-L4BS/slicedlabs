// FAB-INV-TX-12 (SL-ARCH-002 ARCH2-R08, ARCH2-R45; ADR-0019): an expectation that does not hold is
// `conflict` with a report listing footprints, at stage time and at commit; under
// rebase_if_commutes a transaction whose base moved is rebased onto the head only when the
// footprint of every intervening patch is disjoint from its own (the result equals sequential
// application), and an overlap is `conflict` naming the overlapping entries, writing nothing.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

std::string name_in(const state_reader &s, const record_id &id) {
  const auto r = must(s.read(id));
  if (!r) {
    return "<absent>";
  }
  return std::string(must(r->body.get({path_segment::field(1)})).as_string());
}

struct fixture {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  std::unique_ptr<workspace> ws;
  tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  commit_receipt base;
  record_id a = tg::entity_named("a");
  record_id b = tg::entity_named("b");

  fixture() {
    ws = must(
        workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(9)))));
    base = must(tg::commit_ops(ws->manager(), editor,
                               {must(tg::create_entity("a")), must(tg::create_entity("b"))}));
  }

  [[nodiscard]] transaction begin(commit_policy policy) const {
    tx_options o;
    o.origin = editor;
    o.policy = policy;
    return must(ws->manager().begin(o));
  }
};

} // namespace

TEST_CASE("FAB-INV-TX-12: an expect mismatch is conflict with a report listing footprints") {
  fixture f;
  auto t = f.begin(commit_policy::reject_stale);
  patch_op stale = must(tg::set_name(f.a, "a2"));
  stale.expect = expectation::of(must(value::string("not the name")));
  const auto r = t.stage(stale);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::conflict);
  REQUIRE(t.conflict().has_value());
  const conflict_report report = t.conflict().value_or(conflict_report{});
  CHECK(report.code == errc::conflict);
  CHECK(report.ours == footprint_of(stale));
  CHECK(t.state() == tx_state::open); // nothing was staged; the transaction goes on
  must_ok(t.stage(must(tg::set_name(f.a, "a2"))));
  must(t.commit());
}

TEST_CASE("FAB-INV-TX-12: rebase_if_commutes rebases disjoint patches onto the head") {
  fixture f;
  auto late = f.begin(commit_policy::rebase_if_commutes);
  must_ok(late.stage(must(tg::set_name(f.b, "b2"))));
  const commit_receipt first =
      must(tg::commit_ops(f.ws->manager(), f.editor, {must(tg::set_name(f.a, "a2"))}));
  const commit_receipt rebased = must(late.commit());
  const auto rev = must(decode_revision(must(f.store->get(rebased.revision))));
  CHECK(rev.parents == std::vector<digest>{first.revision});
  const snapshot head = must(f.ws->manager().head());
  CHECK(name_in(head, f.a) == "a2");
  CHECK(name_in(head, f.b) == "b2");
  // The stored patch was normalised over the new head.
  const auto tx = must(decode_tx_record(must(f.store->get(rev.tx))));
  const patch stored = must(from_value(must(decode(must(f.store->get(tx.patch)).payload))));
  CHECK(stored.base == first.revision);
}

TEST_CASE("FAB-INV-TX-12: overlapping footprints refuse the rebase with conflict and write "
          "nothing") {
  fixture f;
  auto late = f.begin(commit_policy::rebase_if_commutes);
  const patch_op mine = must(tg::set_name(f.a, "mine"));
  must_ok(late.stage(mine));
  const commit_receipt theirs =
      must(tg::commit_ops(f.ws->manager(), f.editor, {must(tg::set_name(f.a, "theirs"))}));
  const auto r = late.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::conflict);
  CHECK(late.state() == tx_state::conflict);
  REQUIRE(late.conflict().has_value());
  const conflict_report report = late.conflict().value_or(conflict_report{});
  CHECK(report.code == errc::conflict);
  CHECK(report.base == f.base.revision);
  CHECK(report.head == theirs.revision);
  CHECK(report.intervening == std::vector<digest>{theirs.revision});
  REQUIRE_FALSE(report.overlaps.empty());
  CHECK(report.overlaps.front().first.record == f.a);
  CHECK(report.overlaps.front().second.record == f.a);
  CHECK(report.ours == footprint_of(mine));
  CHECK(must(f.store->read_ref("main")) == theirs.revision);
  CHECK(name_in(must(f.ws->manager().head()), f.a) == "theirs");
}

TEST_CASE("FAB-INV-TX-12: a ref reset in between is never rebased across") {
  fixture f;
  auto late = f.begin(commit_policy::rebase_if_commutes);
  must_ok(late.stage(must(tg::set_name(f.b, "b2"))));
  const commit_receipt next =
      must(tg::commit_ops(f.ws->manager(), f.editor, {must(tg::set_name(f.a, "a2"))}));
  must(f.ws->manager().reset_ref("main", f.base.revision, f.editor));
  (void)next;
  const auto r = late.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::conflict);
}
