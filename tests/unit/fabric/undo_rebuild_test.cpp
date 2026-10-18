// FAB-INV-UNDO-10 (SL-ARCH-002 ARCH2-R21; ADR-0018): undo stacks are rebuilt from tx records after
// reopen. The stacks an undo_manager keeps live (its commit observer) equal the stacks replayed
// from history: by rebuild_from_history() on the live manager, and by a new undo_manager after the
// workspace is closed and reopened over the same store, on the memory backend and on the log
// backend over the faulty vfs. Histories mix groups, undos, redos, a barrier, a reset_ref and
// other actors' commits; seeded random sequences of commit, undo, redo and group steps compare
// live and rebuilt stacks after every step, per actor.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"

#include <doctest/doctest.h>

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

patch_op marked(patch_op op, reversibility rev) {
  op.rev = rev;
  return op;
}

struct stacks {
  std::vector<undo_entry> undo;
  std::vector<undo_entry> redo;

  friend bool operator==(const stacks &, const stacks &) = default;
};

stacks of(const undo_manager &um) { return {.undo = um.undo_stack(), .redo = um.redo_stack()}; }

// An undo or redo of a random sequence may be refused, never fail otherwise.
void attempt(const sl::result<commit_receipt> &r) {
  if (!r) {
    CHECK((r.error().code == errc::undo_empty || r.error().code == errc::undo_barrier ||
           r.error().code == errc::undo_conflict));
  }
}

// A history over the test schemas exercising every kind of stack step.
digest scripted_history(transaction_manager &m, undo_manager &editor_um) {
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const tx_origin other = tg::tx_origin_of(tg::tx_other());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const commit_receipt early = must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "a"))}));
  must(tg::commit_ops(m, other, {must(tg::create_entity("bystander"))}));
  must(editor_um.begin_group("level"));
  must(tg::commit_ops(m, editor, {must(tg::create_entity("v1"))}, editor_um.group_meta()));
  must(tg::commit_ops(m, other, {must(tg::create_entity("o1"))}));
  must(tg::commit_ops(m, editor, {must(tg::create_entity("v2"))}, editor_um.group_meta()));
  must_ok(editor_um.end_group());
  must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "b"))}));
  must(editor_um.undo());
  must(editor_um.undo());
  must(editor_um.redo());
  must(tg::commit_ops(m, editor,
                      {marked(must(tg::set_name(hero, "c")), reversibility::irreversible)}));
  must(tg::commit_ops(m, editor, {must(tg::create_entity("after"))}));
  must(tg::commit_ops(m, editor, {must(tg::create_entity("after2"))}));
  must(editor_um.undo());
  // A branch created from an early revision is another ref: main's stacks do not see it.
  must(m.create_ref("side", early.revision, editor));
  must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "side"))}, {}, "side"));
  return early.revision;
}

} // namespace

TEST_CASE("FAB-INV-UNDO-10: undo stacks are rebuilt from tx records after reopen") {
  for (const bool durable : {false, true}) {
    CAPTURE(durable);
    tg::tx_log_store log;
    std::shared_ptr<store_backend> store =
        durable ? must(log.open())
                : std::shared_ptr<store_backend>(std::make_shared<memory_backend>());
    stacks live_editor;
    stacks live_other;
    digest early;
    {
      auto ws = must(workspace::create_with(
          store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
      transaction_manager &m = ws->manager();
      undo_manager editor(m, tg::tx_editor());
      undo_manager other(m, tg::tx_other());
      early = scripted_history(m, editor);
      live_editor = of(editor);
      live_other = of(other);
      CHECK(live_editor.undo.size() == 2); // the barrier entry and "after"
      CHECK(live_editor.undo.front().barrier.has_value());
      CHECK(live_editor.redo.size() == 1);
      CHECK(live_other.undo.size() == 2);
      // Replaying history on the live manager gives the same stacks.
      must_ok(editor.rebuild_from_history(must(m.head())));
      CHECK(of(editor) == live_editor);
      // An undo_manager of the "side" ref sees that ref's history only.
      undo_manager side(m, tg::tx_editor(),
                        undo_options{.ref = "side", .intents = nullptr, .compensators = nullptr});
      CHECK(side.undo_stack().size() == 3); // create hero, rename a, rename side
    }
    if (durable) {
      store.reset();
      store = must(log.open());
    }
    auto ws = must(
        workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
    transaction_manager &m = ws->manager();
    undo_manager editor(m, tg::tx_editor());
    undo_manager other(m, tg::tx_other());
    CHECK(of(editor) == live_editor);
    CHECK(of(other) == live_other);
    // The rebuilt stacks work: redo, undo, and the barrier still stops the stack.
    must(editor.redo());
    must(editor.undo());
    must(editor.undo());
    CHECK(editor.undo().error().code == errc::undo_barrier);
    // reset_ref on main moves back to an older state: the stacks follow the new first parents.
    must(m.reset_ref("main", early, tg::tx_origin_of(tg::tx_other())));
    undo_manager fresh(m, tg::tx_editor());
    CHECK(of(editor) == of(fresh));
    CHECK(editor.undo_stack().size() == 2); // create hero, rename a
    CHECK(editor.redo_stack().empty());
  }
}

TEST_CASE("FAB-INV-UNDO-10: live and rebuilt stacks agree after every step (seeded)") {
  const std::size_t seeds = tg::sweep_scaled(20);
  std::size_t steps = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    opts.max_ops = 3;
    tg::patch_gen gen(seed + 500, opts);
    tg::oracle_store oracle = must(gen.state(4));
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager editor(m, tg::tx_editor());
    undo_manager other(m, tg::tx_other());
    bool grouped = false;
    for (int i = 0; i < 14; ++i) {
      CAPTURE(i);
      const std::uint64_t pick = gen.random().below(7);
      undo_manager &who = pick == 6 ? other : editor;
      if (pick <= 2) {
        oracle = must(tg::oracle_of(must(m.head())));
        patch p = must(gen.next(oracle));
        p.base.reset();
        tx_options o;
        o.origin = tg::tx_origin_of(who.actor());
        o.meta = who.group_meta();
        auto t = must(m.begin(o));
        must_ok(t.stage(p));
        must(t.commit());
      } else if (pick == 3) {
        attempt(editor.undo());
      } else if (pick == 4) {
        attempt(editor.redo());
      } else if (pick == 5) {
        if (grouped) {
          must_ok(editor.end_group());
        } else {
          must(editor.begin_group("g"));
        }
        grouped = !grouped;
      } else {
        oracle = must(tg::oracle_of(must(m.head())));
        patch p = must(gen.next(oracle));
        p.base.reset();
        must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), p.ops));
      }
      for (const undo_manager *um : {&editor, &other}) {
        undo_manager replay(m, um->actor());
        CHECK(of(replay) == of(*um));
      }
      ++steps;
    }
  }
  CHECK(steps == seeds * 14);
}
