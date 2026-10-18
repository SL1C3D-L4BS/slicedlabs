// FAB-INV-UNDO-04 (SL-ARCH-002 ARCH2-R20; ADR-0018): undo is a new committed transaction carrying
// undo_of, and history only grows. Over the test schemas an undo adds one revision whose tx record
// names the undone tx and whose patch object lists it; the undone revisions stay readable. Over
// seeded patch_gen patches, undo after commit restores the head to the oracle state before the
// commit, LIFO, until the actor's stack is empty.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
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

patch_meta labeled(std::string label) {
  patch_meta m;
  m.label = std::move(label);
  return m;
}

std::string name_of(const transaction_manager &m, const record_id &id) {
  const snapshot head = must(m.head());
  const snapshot_entry e = must(head.get(id));
  const value *name = e.body.find_field(1);
  REQUIRE(name != nullptr);
  return std::string(name->as_string());
}

} // namespace

TEST_CASE("FAB-INV-UNDO-04: undo is a committed transaction with undo_of; history only grows") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  CHECK_FALSE(um.can_undo());
  CHECK(um.undo().error().code == errc::undo_empty);
  CHECK(um.redo().error().code == errc::undo_empty);

  const commit_receipt created =
      must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}, labeled("create hero")));
  const std::string original = name_of(m, hero);
  const commit_receipt a =
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "a"))}, labeled("rename a")));
  const commit_receipt b =
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "b"))}, labeled("rename b")));
  CHECK(um.can_undo());
  CHECK(um.undo_label() == "rename b");
  CHECK(um.undo_stack().size() == 3);

  const std::size_t before = must(tg::history_length(*store, b.revision));
  const std::size_t objects = store->object_count();
  const commit_receipt u = must(um.undo());
  // A new revision on top of the old head: nothing rewritten, history grew by one.
  CHECK(must(tg::history_length(*store, u.revision)) == before + 1);
  CHECK(store->object_count() > objects);
  CHECK(must(m.head()).revision_digest() == u.revision);
  CHECK(must(store->has(b.revision)));
  CHECK(must(m.at(b.revision)).root_digest() == b.root);
  CHECK(name_of(m, hero) == "a");
  CHECK(u.root == a.root); // a set undone restores the exact state
  const auto tx = must(tg::stored_tx(*store, u.revision));
  CHECK(tx.undo_of == std::optional<record_id>(b.tx.id()));
  CHECK(tx.label == "undo rename b");
  CHECK(tx.actor == tg::tx_editor().id());
  const patch p = must(tg::stored_patch(*store, u.revision));
  CHECK(p.meta.undo_of == std::vector<tx_id>{b.tx});
  CHECK(p.base == std::optional<digest>(b.revision));
  CHECK(um.can_redo());
  CHECK(um.undo_label() == "rename a");

  CHECK(must(um.undo()).root == created.root);
  CHECK(name_of(m, hero) == original);
  // Undo of the create retires the id.
  must(um.undo());
  CHECK_FALSE(must(must(m.head()).contains(hero)));
  CHECK_FALSE(um.can_undo());
  CHECK(um.undo().error().code == errc::undo_empty);
  CHECK(um.redo_stack().size() == 3);
  CHECK(m.commits() == 6);
}

TEST_CASE("FAB-INV-UNDO-04: undo after commit restores the oracle (seeded patch_gen)") {
  const std::size_t seeds = tg::sweep_scaled(40);
  std::size_t undone = 0;
  std::size_t entries = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    tg::patch_gen gen(seed, opts);
    tg::oracle_store oracle = must(gen.state(4));
    // The load is another actor's: it is not on the editor's stack.
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());
    CHECK_FALSE(um.can_undo());

    std::vector<tg::oracle_store> states{oracle};
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    for (int i = 0; i < 5; ++i) {
      patch p = must(gen.next(oracle));
      p.base.reset();
      auto t = must(m.begin(o));
      must_ok(t.stage(p));
      must(t.commit());
      must_ok(oracle.apply(p));
      REQUIRE(must(tg::oracle_of(must(m.head()))) == oracle);
      // An annotate-only patch changes nothing and makes no entry.
      if (std::ranges::any_of(p.ops,
                              [](const patch_op &op) { return op.kind != op_kind::annotate; })) {
        states.push_back(oracle);
      }
    }
    CHECK(um.undo_stack().size() == states.size() - 1);
    entries += states.size() - 1;
    for (std::size_t i = states.size() - 1; i > 0; --i) {
      CAPTURE(i);
      const std::size_t len = must(tg::history_length(*store, must(m.head()).revision_digest()));
      const commit_receipt r = must(um.undo());
      CHECK(must(tg::history_length(*store, r.revision)) == len + 1);
      CHECK(must(tg::oracle_of(must(m.head()))) == states[i - 1]);
      ++undone;
    }
    CHECK(um.undo().error().code == errc::undo_empty);
  }
  CHECK(undone == entries);
  CHECK(entries >= seeds * 4);
}
