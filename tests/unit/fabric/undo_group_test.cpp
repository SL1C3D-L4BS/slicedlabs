// FAB-INV-UNDO-05 (SL-ARCH-002 ARCH2-R22; ADR-0018): a group undoes as one transaction or not at
// all. Transactions begun with group_meta() between begin_group and end_group form one entry,
// even with another actor's commit in between and after a commit of the group failed at an
// injected failpoint. Undo with a failpoint armed (FP0, FP1, FP2) or a store step failing mid
// batch leaves head, objects and stacks as they were; the next undo reverts the whole group in
// one revision whose patch lists every tx. Seeded patch_gen groups restore the oracle.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/store_faults.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

bool changes_state(const patch &p) {
  return std::ranges::any_of(p.ops,
                             [](const patch_op &op) { return op.kind != op_kind::annotate; });
}

} // namespace

TEST_CASE("FAB-INV-UNDO-05: a group undoes atomically under injected failure") {
  auto faults = std::make_shared<tg::store_faults>();
  auto store = std::make_shared<memory_backend>(faults);
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const tg::oracle_store before = must(tg::oracle_of(must(m.head())));

  const record_id group = must(um.begin_group("build level"));
  CHECK(must(um.begin_group("nested")) == group); // nested groups join the open one
  must_ok(um.end_group());
  CHECK(um.group_meta().undo_group == group);
  CHECK(um.group_meta().label == "build level");
  std::vector<tx_id> group_txs;
  group_txs.push_back(
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "one"))}, um.group_meta())).tx);
  // Another actor's commit in the middle does not split the group.
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), {must(tg::create_entity("bystander"))}));
  group_txs.push_back(
      must(tg::commit_ops(m, editor, {must(tg::create_entity("villain"))}, um.group_meta())).tx);
  // A commit of the group fails at FP1: the group keeps the commits that landed.
  must_ok(tg::tx_failpoints::arm(m.failpoints(), failpoint::fp1));
  CHECK(
      tg::commit_ops(m, editor, {must(tg::set_name(hero, "lost"))}, um.group_meta()).error().code ==
      errc::store_io);
  group_txs.push_back(
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "three"))}, um.group_meta())).tx);
  must_ok(um.end_group());
  CHECK(um.end_group().error().code == errc::invalid_argument);
  CHECK(um.group_meta().undo_group.is_null());

  REQUIRE(um.undo_stack().size() == 2);
  CHECK(um.undo_stack().back().txs == group_txs);
  CHECK(um.undo_stack().back().group == group);
  CHECK(um.undo_label() == "build level");

  const snapshot head = must(m.head());
  const auto stacks = um.undo_stack();
  const auto objects = store->object_count();
  const auto unchanged = [&] {
    CHECK(must(m.head()).revision_digest() == head.revision_digest());
    CHECK(store->object_count() == objects);
    CHECK(um.undo_stack() == stacks);
    CHECK(um.redo_stack().empty());
  };
  for (const failpoint fp : {failpoint::fp0, failpoint::fp1, failpoint::fp2}) {
    CAPTURE(to_string(fp));
    must_ok(tg::tx_failpoints::arm(m.failpoints(), fp));
    CHECK(um.undo().error().code == errc::store_io);
    unchanged();
  }
  // A store step fails in the middle of the batch: all or nothing.
  faults->fail_after(2);
  CHECK(um.undo().error().code == errc::store_io);
  faults->disarm();
  unchanged();

  const std::size_t len = must(tg::history_length(*store, head.revision_digest()));
  const commit_receipt r = must(um.undo());
  CHECK(must(tg::history_length(*store, r.revision)) == len + 1); // one transaction
  CHECK(must(tg::stored_patch(*store, r.revision)).meta.undo_of == group_txs);
  CHECK(must(tg::stored_tx(*store, r.revision)).undo_of == group_txs.front().id());
  // Everything of the group is reverted; the bystander stays.
  tg::oracle_store expected = before;
  const snapshot after = must(m.head());
  expected.put(tg::entity_named("bystander"),
               record_state{.type = tg::entity_type(),
                            .body = must(after.get(tg::entity_named("bystander"))).body});
  CHECK(must(tg::oracle_of(after)) == expected);
  CHECK(um.undo_stack().size() == 1);
  CHECK(um.redo_stack().size() == 1);
}

TEST_CASE("FAB-INV-UNDO-05: a seeded patch_gen group undoes as one transaction") {
  const std::size_t seeds = tg::sweep_scaled(30);
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    tg::patch_gen gen(seed + 100, opts);
    tg::oracle_store oracle = must(gen.state(4));
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());
    const tg::oracle_store start = oracle;

    must(um.begin_group("seeded"));
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    o.meta = um.group_meta();
    std::size_t members = 0;
    for (int i = 0; i < 4; ++i) {
      patch p = must(gen.next(oracle));
      p.base.reset();
      auto t = must(m.begin(o));
      must_ok(t.stage(p));
      must(t.commit());
      must_ok(oracle.apply(p));
      if (changes_state(p)) {
        ++members;
      }
    }
    must_ok(um.end_group());
    if (members == 0) {
      CHECK_FALSE(um.can_undo());
      continue;
    }
    REQUIRE(um.undo_stack().size() == 1);
    CHECK(um.undo_stack().front().txs.size() == members);
    const std::size_t len = must(tg::history_length(*store, must(m.head()).revision_digest()));
    const commit_receipt r = must(um.undo());
    CHECK(must(tg::history_length(*store, r.revision)) == len + 1);
    CHECK(must(tg::oracle_of(must(m.head()))) == start);
    CHECK_FALSE(um.can_undo());
  }
}
