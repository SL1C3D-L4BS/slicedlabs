// FAB-INV-UNDO-09 (SL-ARCH-002 ARCH2-R23; ADR-0018): an intervening edit makes undo undo_conflict
// and writes nothing. Another actor changing a value the inverse expects (or deleting its record)
// refuses the undo: head, objects and stacks stay, and last_conflict() reports the inverse's
// footprint, the overlapping entries and the intervening revision. A disjoint intervening edit
// does not block the undo. Over seeded patch_gen histories the oracle decides: when the inverted
// patch still applies to the oracle after the other actor's patch, undo succeeds and matches it;
// otherwise undo is refused and the head is unchanged.

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

} // namespace

TEST_CASE("FAB-INV-UNDO-09: an intervening edit makes undo undo_conflict and writes nothing") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const tx_origin other = tg::tx_origin_of(tg::tx_other());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "mine"))}));
  const commit_receipt theirs =
      must(tg::commit_ops(m, other, {must(tg::set_name(hero, "theirs"))}));

  const auto objects = store->object_count();
  const auto head = must(m.head()).revision_digest();
  const auto stacks = um.undo_stack();
  auto refused = um.undo();
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::undo_conflict);
  CHECK(store->object_count() == objects);
  CHECK(must(m.head()).revision_digest() == head);
  CHECK(um.undo_stack() == stacks);
  CHECK(um.redo_stack().empty());
  REQUIRE(um.last_conflict().has_value());
  const conflict_report report = um.last_conflict().value_or(conflict_report{});
  CHECK(report.code == errc::undo_conflict);
  CHECK(report.head == head);
  CHECK(report.intervening == std::vector<digest>{theirs.revision});
  CHECK(std::ranges::any_of(report.ours.entries, [&](const footprint_entry &e) {
    return e.record == hero && e.mode == access::write;
  }));
  REQUIRE_FALSE(report.overlaps.empty());
  CHECK(report.overlaps.front().first.record == hero);
  CHECK(report.overlaps.front().second.record == hero);

  // A disjoint edit by the other actor does not block undo of the create.
  const commit_receipt c2 = must(tg::commit_ops(m, editor, {must(tg::create_entity("prop"))}));
  must(tg::commit_ops(m, other, {must(tg::create_entity("bystander"))}));
  must(um.undo());
  CHECK_FALSE(must(must(m.head()).contains(tg::entity_named("prop"))));
  CHECK_FALSE(um.last_conflict().has_value());
  (void)c2;

  // The other actor deletes the record the inverse writes: refused as well.
  must(tg::commit_ops(m, other, {op_delete(hero)}));
  CHECK(um.undo().error().code == errc::undo_conflict);
  CHECK(um.last_conflict().has_value());
}

TEST_CASE("FAB-INV-UNDO-09: the oracle decides between undo and undo_conflict (seeded patch_gen)") {
  const std::size_t seeds = 60; // small states: cheap under the sanitizers too
  std::size_t refused = 0;
  std::size_t undone = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    opts.max_ops = 3;
    tg::patch_gen gen(seed + 400, opts);
    tg::oracle_store oracle = must(gen.state(3));
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());

    patch mine = must(gen.next(oracle));
    mine.base.reset();
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    auto t = must(m.begin(o));
    must_ok(t.stage(mine));
    const commit_receipt c = must(t.commit());
    must_ok(oracle.apply(mine));
    if (!um.can_undo()) {
      continue; // annotate only
    }
    patch theirs = must(gen.next(oracle));
    theirs.base.reset();
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), theirs.ops));
    must_ok(oracle.apply(theirs));

    // What the oracle says the undo does: the stored (normalised) patch inverted, over it.
    patch stored = must(tg::stored_patch(*store, c.revision));
    std::erase_if(stored.ops, [](const patch_op &op) { return op.kind == op_kind::annotate; });
    tg::oracle_store expected = oracle;
    const bool applies = expected.apply(invert(stored)).has_value();
    const auto head = must(m.head()).revision_digest();
    auto r = um.undo();
    CHECK(r.has_value() == applies);
    if (r) {
      CHECK(must(tg::oracle_of(must(m.head()))) == expected);
      ++undone;
    } else {
      CHECK(r.error().code == errc::undo_conflict);
      CHECK(must(m.head()).revision_digest() == head);
      CHECK(um.can_undo());
      ++refused;
    }
  }
  MESSAGE("undone " << undone << " refused " << refused);
  CHECK(undone > 0);
  CHECK(refused > 0);
}
