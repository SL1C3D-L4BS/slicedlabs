// FAB-INV-UNDO-06 (SL-ARCH-002 ARCH2-R25; ADR-0018): redo restores the exact root and is cleared by
// the actor's next non-undo commit. Redo is itself a new transaction whose undo_of names the undo
// it reverts; redo of an edit meets the commit's root exactly. Tombstones are history (ARCH1-R05):
// an id retired by an undo stays retired and a redone delete retires the id under the redo's tx
// id, so where retirements differ the records tries must differ in tombstones only (same live
// record objects, same last records). Over seeded patch_gen histories, undoing everything and
// redoing everything meets every commit state; another actor's commit does not clear the redo
// stack, the actor's own does.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
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

TEST_CASE("FAB-INV-UNDO-06: redo restores the exact root and is cleared by a new non-undo commit") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  const commit_receipt c0 = must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const commit_receipt c1 = must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "a"))}));
  const commit_receipt c2 = must(tg::commit_ops(m, editor, {op_delete(hero)}));

  const commit_receipt u2 = must(um.undo()); // revives hero
  CHECK(u2.root == c1.root);
  const commit_receipt u1 = must(um.undo());
  CHECK(u1.root == c0.root);
  CHECK(um.redo_stack().size() == 2);
  CHECK(um.can_redo());

  const commit_receipt r1 = must(um.redo());
  CHECK(r1.root == c1.root);
  const patch rp = must(tg::stored_patch(*store, r1.revision));
  CHECK(rp.meta.undo_of == std::vector<tx_id>{u1.tx});
  CHECK(must(tg::stored_tx(*store, r1.revision)).label ==
        "redo " + must(tg::stored_tx(*store, c1.revision)).label);
  // Redo of the delete retires hero again: the tombstone keeps the same last record object and
  // names the redo as the retiring tx, the only difference from the first delete.
  const commit_receipt r2 = must(um.redo());
  CHECK(must(tg::same_up_to_tombstones(must(m.at(r2.revision)), must(m.at(c2.revision)))));
  const auto tomb = must(must(m.at(r2.revision)).entry(hero));
  REQUIRE(tomb.has_value());
  const tree_entry leaf = tomb.value_or(tree_entry{});
  CHECK(must(decode_tombstone(must(store->get(leaf.target)))).retired_by == r2.tx.id());
  CHECK_FALSE(um.can_redo());
  CHECK(um.redo().error().code == errc::undo_empty);
  // The redone edits are undoable again, and redoable again.
  CHECK(must(um.undo()).root == c1.root);
  CHECK(must(
      tg::same_up_to_tombstones(must(m.at(must(um.redo()).revision)), must(m.at(c2.revision)))));

  // Another actor's commit leaves the redo stack; the actor's own non-undo commit clears it.
  must(um.undo());
  REQUIRE(um.can_redo());
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), {must(tg::create_entity("other"))}));
  CHECK(um.can_redo());
  must(tg::commit_ops(m, editor, {must(tg::create_entity("fresh"))}));
  CHECK_FALSE(um.can_redo());
  CHECK(um.redo_stack().empty());
  CHECK(um.redo().error().code == errc::undo_empty);
  // The undone revisions stay in history.
  for (const commit_receipt &c : {c0, c1, c2, u1, u2, r1, r2}) {
    CHECK(must(store->has(c.revision)));
  }
}

TEST_CASE("FAB-INV-UNDO-06: undo all then redo all meets every commit root (seeded patch_gen)") {
  const std::size_t seeds = tg::sweep_scaled(30);
  std::size_t redone = 0;
  std::size_t exact = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    tg::patch_gen gen(seed + 200, opts);
    tg::oracle_store oracle = must(gen.state(4));
    const commit_receipt load =
        must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());

    std::vector<commit_receipt> commits{load};
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    for (int i = 0; i < 4; ++i) {
      patch p = must(gen.next(oracle));
      p.base.reset();
      auto t = must(m.begin(o));
      must_ok(t.stage(p));
      const commit_receipt c = must(t.commit());
      must_ok(oracle.apply(p));
      if (changes_state(p)) {
        commits.push_back(c);
      }
    }
    const std::size_t n = commits.size() - 1;
    for (std::size_t i = 0; i < n; ++i) {
      must(um.undo());
    }
    CHECK_FALSE(um.can_undo());
    CHECK(um.redo_stack().size() == n);
    for (std::size_t i = 1; i <= n; ++i) {
      CAPTURE(i);
      const commit_receipt r = must(um.redo());
      // The exact root, unless tombstones differ: an id retired by an undo stays retired, and a
      // redo retires an id again under its own tx id.
      if (r.root == commits[i].root) {
        ++exact;
      } else {
        CHECK(must(
            tg::same_up_to_tombstones(must(m.at(r.revision)), must(m.at(commits[i].revision)))));
      }
      ++redone;
    }
    CHECK(must(tg::oracle_of(must(m.head()))) == oracle);
    CHECK_FALSE(um.can_redo());
    if (n > 0) {
      must(um.undo());
      CHECK(um.can_redo());
      const tg::oracle_store now = must(tg::oracle_of(must(m.head())));
      REQUIRE(now.size() > 0);
      must(tg::commit_ops(m, o.origin, {op_delete(now.records().begin()->first)}));
      CHECK_FALSE(um.can_redo());
    }
  }
  MESSAGE("redone " << redone << " exact root " << exact);
  CHECK(redone > seeds * 2);
  CHECK(exact > redone / 4);
}
