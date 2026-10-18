// FAB-INV-UNDO-12 (SL-ARCH-002 ARCH2-R27; ADR-0018, ADR-0011): delete then undo restores the same
// id and record object id. Undo of a delete re-creates the captured body under the same id (the
// only revival the structural validator accepts: a plain create over the tombstone is id_reuse) and
// the records trie points at the same record object as before the delete; redo retires it again.
// Seeded patch_gen states: every record deleted and undone comes back with its record object id.

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

#include <cstddef>
#include <cstdint>
#include <map>
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

} // namespace

TEST_CASE("FAB-INV-UNDO-12: delete then undo restores the same id and record object id") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "named"))}));
  const snapshot_entry live = must(must(m.head()).get(hero));

  must(tg::commit_ops(m, editor, {op_delete(hero)}));
  const snapshot deleted = must(m.head());
  CHECK_FALSE(must(deleted.contains(hero)));
  const auto tomb = must(deleted.entry(hero));
  REQUIRE(tomb.has_value());
  CHECK(tomb.value_or(tree_entry{}).kind == entry_kind::tombstone);
  // Only an undo may revive a retired id.
  CHECK(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}).error().code ==
        errc::validation_failed);

  must(um.undo());
  const snapshot_entry back = must(must(m.head()).get(hero));
  CHECK(back.id == hero);
  CHECK(back.object == live.object);
  CHECK(back == live);
  // Redo retires it again; undo revives it again, with the same object.
  must(um.redo());
  CHECK_FALSE(must(must(m.head()).contains(hero)));
  must(um.undo());
  CHECK(must(must(m.head()).get(hero)).object == live.object);
}

TEST_CASE("FAB-INV-UNDO-12: deleted seeded records come back with their record object ids") {
  const std::size_t seeds = tg::sweep_scaled(30);
  std::size_t revived = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto store = std::make_shared<memory_backend>();
    auto ws = must(workspace::create_with(
        store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    tg::patch_gen gen(seed + 600);
    const tg::oracle_store oracle = must(gen.state(5));
    must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_other()), tg::load_patch(oracle).ops));
    undo_manager um(m, tg::tx_editor());
    std::map<record_id, digest> objects;
    const snapshot head = must(m.head());
    for (const auto &[id, r] : oracle.records()) {
      objects.emplace(id, must(head.get(id)).object);
    }
    // Delete them one per transaction, then everything at once in a group of two.
    std::vector<patch_op> deletes;
    for (const auto &[id, r] : oracle.records()) {
      deletes.push_back(op_delete(id));
    }
    const std::size_t half = deletes.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
      must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {deletes[i]}));
    }
    must(tg::commit_ops(
        m, tg::tx_origin_of(tg::tx_editor()),
        std::vector<patch_op>(deletes.begin() + static_cast<std::ptrdiff_t>(half), deletes.end())));
    CHECK(must(tg::oracle_of(must(m.head()))).size() == 0);
    while (um.can_undo()) {
      must(um.undo());
    }
    const snapshot after = must(m.head());
    for (const auto &[id, object] : objects) {
      CHECK(must(after.get(id)).object == object);
      ++revived;
    }
    CHECK(must(tg::oracle_of(after)) == oracle);
  }
  CHECK(revived == seeds * 5);
}
