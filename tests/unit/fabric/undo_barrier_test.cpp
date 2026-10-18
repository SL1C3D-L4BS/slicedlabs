// FAB-INV-UNDO-07 (SL-ARCH-002 ARCH2-R24; ADR-0018): an irreversible op is a barrier. Undo of its
// entry is refused with undo_barrier naming the op (index, kind, tx, target), writes nothing, and
// nothing below it stays reachable; edits made after it undo normally and stop at it again. A
// compensable mutation whose entry declares no compensation is a barrier too, lifted once a
// compensation joins the group. Seeded patch_gen histories with one op marked irreversible never
// undo past it.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

patch_op marked(patch_op op, reversibility rev) {
  op.rev = rev;
  return op;
}

} // namespace

TEST_CASE("FAB-INV-UNDO-07: an irreversible op is a barrier: undo_barrier names it") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "a"))}));
  const commit_receipt published = must(
      tg::commit_ops(m, editor,
                     {op_annotate(value::uint(1)),
                      marked(must(tg::set_name(hero, "published")), reversibility::irreversible)}));
  // The barrier drops everything below it.
  REQUIRE(um.undo_stack().size() == 1);
  const undo_entry top = um.undo_stack().back();
  REQUIRE(top.barrier.has_value());
  const undo_barrier_info barrier = top.barrier.value_or(undo_barrier_info{});
  CHECK(barrier.tx == published.tx);
  CHECK(barrier.op_index == 1);
  CHECK(barrier.kind == op_kind::set);
  CHECK(barrier.target == hero);
  CHECK(barrier.rev == reversibility::irreversible);
  CHECK_FALSE(um.can_undo());
  CHECK_FALSE(um.undo_label().has_value());

  const auto objects = store->object_count();
  const auto head = must(m.head()).revision_digest();
  auto refused = um.undo();
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::undo_barrier);
  CHECK(refused.error().message.find("irreversible") != std::string::npos);
  CHECK(refused.error().message.find(published.tx.to_string()) != std::string::npos);
  CHECK(refused.error().message.find(hero.to_string()) != std::string::npos);
  CHECK(refused.error().message.find("(set)") != std::string::npos);
  CHECK(store->object_count() == objects);
  CHECK(must(m.head()).revision_digest() == head);

  // Edits after the barrier undo normally and stop at it.
  must(tg::commit_ops(m, editor, {must(tg::create_entity("later"))}));
  CHECK(um.can_undo());
  must(um.undo());
  CHECK(um.undo().error().code == errc::undo_barrier);
  // Redo still works above the barrier.
  must(um.redo());
  CHECK(must(must(m.head()).contains(tg::entity_named("later"))));
}

TEST_CASE("FAB-INV-UNDO-07: a compensable mutation without a compensation is a barrier") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));

  must(um.begin_group("upload"));
  must(tg::commit_ops(m, editor,
                      {marked(must(tg::set_name(hero, "uploaded")), reversibility::compensable)},
                      um.group_meta()));
  const std::optional<undo_barrier_info> uncovered = um.undo_stack().back().barrier;
  REQUIRE(uncovered.has_value());
  CHECK(uncovered.value_or(undo_barrier_info{}).rev == reversibility::compensable);
  auto refused = um.undo();
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::undo_barrier);
  CHECK(refused.error().message.find("compensable") != std::string::npos);
  // The compensation joins the group: the barrier is lifted; the entry below is still there.
  must(tg::commit_ops(
      m, editor,
      {must(op_compensation(compensation{.command = "test.unupload", .args = value::id(hero)}))},
      um.group_meta()));
  must_ok(um.end_group());
  CHECK(um.undo_stack().size() == 2);
  CHECK_FALSE(um.undo_stack().back().barrier.has_value());
  CHECK(um.undo_stack().back().compensations == 1);
  CHECK(um.undo_stack().back().compensable_mutations == 1);
  CHECK(um.can_undo());
}

TEST_CASE("FAB-INV-UNDO-07: seeded histories never undo past an irreversible op") {
  const std::size_t seeds = tg::sweep_scaled(30);
  std::size_t barriers = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    tg::patch_gen gen(seed + 300, opts);
    tg::oracle_store oracle = must(gen.state(4));
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    std::size_t after_barrier = 0;
    bool barrier = false;
    for (int i = 0; i < 5; ++i) {
      patch p = must(gen.next(oracle));
      p.base.reset();
      const bool mutates = std::ranges::any_of(
          p.ops, [](const patch_op &op) { return op.kind != op_kind::annotate; });
      if (i == 2) {
        p.ops.back().rev = reversibility::irreversible;
        barrier = true;
        after_barrier = 0;
      } else if (barrier && mutates) {
        ++after_barrier;
      }
      auto t = must(m.begin(o));
      must_ok(t.stage(p));
      must(t.commit());
      must_ok(oracle.apply(p));
    }
    for (std::size_t i = 0; i < after_barrier; ++i) {
      must(um.undo());
    }
    const auto head = must(m.head()).revision_digest();
    CHECK(um.undo().error().code == errc::undo_barrier);
    CHECK(must(m.head()).revision_digest() == head);
    CHECK(um.undo_stack().size() == 1);
    ++barriers;
  }
  CHECK(barriers == seeds);
}
