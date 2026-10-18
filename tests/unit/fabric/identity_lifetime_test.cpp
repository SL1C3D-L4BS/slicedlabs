// FAB-INV-LC-01 (SL-ARCH-001 ARCH1-R04, ARCH1-R05; ADR-0011, ADR-0018): an id is unborn, then live,
// then tombstoned for good. A write under a tombstoned id is refused on commit (R-INV-ID-REUSE,
// id_reuse) unless the patch's undo_of names the transaction that retired it: a plain create, or
// an undo_of naming any other transaction, is refused; the undo of the retiring transaction
// revives the same id. Undo and redo keep the rule (WO-052): an undone create is retired by the
// undo and revived by the redo; a redone delete is retired by the redo and revived by the next
// undo. Seeded patch_gen histories over the memory backend: a tombstone never goes away except by
// the undo of its retirer, and every refused revival leaves the head where it was.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

patch_meta undo_of(std::vector<tx_id> txs) {
  patch_meta m;
  m.undo_of = std::move(txs);
  return m;
}

bool reports_reuse(std::span<const diagnostic> d, const record_id &id) {
  return std::ranges::any_of(d, [&](const diagnostic &x) {
    return x.rule == "R-INV-ID-REUSE" && x.stage == validator_stage::invariants && x.record == id &&
           x.message.starts_with("id_reuse");
  });
}

// Begins a transaction staging `ops` with `meta`, commits it and returns it (state and diagnostics
// stay readable).
transaction attempt(transaction_manager &m, const tx_origin &origin,
                    const std::vector<patch_op> &ops, patch_meta meta) {
  tx_options o;
  o.origin = origin;
  o.meta = std::move(meta);
  auto t = must(m.begin(std::move(o)));
  for (const patch_op &op : ops) {
    must_ok(t.stage(op));
  }
  [[maybe_unused]] auto r = t.commit();
  return t;
}

id_lifetime life(transaction_manager &m, const record_id &id) {
  return must(lifetime_of(must(m.head()), id));
}

record_id retirer(transaction_manager &m, const record_id &id) {
  const std::optional<tombstone> t = must(tombstone_of(must(m.head()), id));
  REQUIRE(t.has_value());
  return t.value_or(tombstone{}).retired_by;
}

} // namespace

TEST_CASE("FAB-INV-LC-01: a retired id is revived only by the undo of the retiring transaction") {
  auto ws =
      must(workspace::create_with(std::make_shared<memory_backend>(),
                                  must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  CHECK(life(m, hero) == id_lifetime::unborn);
  CHECK_FALSE(must(tombstone_of(must(m.head()), hero)).has_value());
  const commit_receipt created = must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  CHECK(life(m, hero) == id_lifetime::live);
  const commit_receipt deleted = must(tg::commit_ops(m, editor, {op_delete(hero)}));
  CHECK(life(m, hero) == id_lifetime::tombstoned);
  CHECK(retirer(m, hero) == deleted.tx.id());

  const std::optional<digest> head = must(m.head()).revision_digest();
  const tx_id stranger = must(tx_id::from(well_known(id_class::tx, 77)));
  // A plain create, and undo_of naming the creating transaction or an unrelated one: id_reuse.
  for (const patch_meta &meta : {patch_meta{}, undo_of({created.tx}), undo_of({stranger}),
                                 undo_of({stranger, created.tx})}) {
    CAPTURE(meta.undo_of.size());
    transaction t = attempt(m, editor, {must(tg::create_entity("hero"))}, meta);
    CHECK(t.state() == tx_state::rejected);
    CHECK(reports_reuse(t.diagnostics(), hero));
    CHECK(must(m.head()).revision_digest() == head);
  }
  // The dry run reports the same.
  {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::create_entity("hero"))));
    CHECK(reports_reuse(must(t.validate()), hero));
  }
  // A write under the id with undo_of naming the retirer (alone or among others) revives it.
  transaction t =
      attempt(m, editor, {must(tg::create_entity("hero"))}, undo_of({stranger, deleted.tx}));
  CHECK(t.state() == tx_state::committed);
  CHECK_FALSE(reports_reuse(t.diagnostics(), hero));
  CHECK(life(m, hero) == id_lifetime::live);
  // Retired again: the new tombstone names the new retirer, and the old undo_of no longer works.
  const commit_receipt again = must(tg::commit_ops(m, editor, {op_delete(hero)}));
  CHECK(retirer(m, hero) == again.tx.id());
  CHECK(attempt(m, editor, {must(tg::create_entity("hero"))}, undo_of({deleted.tx})).state() ==
        tx_state::rejected);
}

TEST_CASE("FAB-INV-LC-01: undo and redo revive only what their undo_of retired") {
  auto ws =
      must(workspace::create_with(std::make_shared<memory_backend>(),
                                  must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  // An undone create leaves a tombstone retired by the undo; only the redo revives it.
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const commit_receipt u1 = must(um.undo());
  CHECK(life(m, hero) == id_lifetime::tombstoned);
  CHECK(retirer(m, hero) == u1.tx.id());
  const commit_receipt r1 = must(um.redo());
  CHECK(life(m, hero) == id_lifetime::live);
  CHECK(must(tg::stored_tx(m.store(), r1.revision)).undo_of == std::optional(u1.tx.id()));

  // A delete undone and redone: the redo is the retirer, and the next undo revives the id.
  const commit_receipt d = must(tg::commit_ops(m, editor, {op_delete(hero)}));
  CHECK(retirer(m, hero) == d.tx.id());
  must(um.undo());
  CHECK(life(m, hero) == id_lifetime::live);
  const commit_receipt r2 = must(um.redo());
  CHECK(retirer(m, hero) == r2.tx.id());
  // The original delete no longer licenses a revival.
  CHECK(attempt(m, editor, {must(tg::create_entity("hero"))}, undo_of({d.tx})).state() ==
        tx_state::rejected);
  must(um.undo());
  CHECK(life(m, hero) == id_lifetime::live);
}

TEST_CASE("FAB-INV-LC-01: seeded histories never revive a tombstone but by undo of its retirer") {
  const std::size_t seeds = tg::sweep_scaled(20);
  std::size_t refused = 0;
  std::size_t revived = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto ws = must(
        workspace::create_with(std::make_shared<memory_backend>(),
                               must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
    tg::patch_gen gen(seed + 900);
    tg::oracle_store oracle = must(gen.state(6));
    const commit_receipt load = must(tg::commit_ops(m, editor, tg::load_patch(oracle).ops));

    // Retire every other record in one transaction.
    std::vector<record_id> retired;
    std::vector<patch_op> deletes;
    std::size_t i = 0;
    for (const auto &[id, r] : oracle.records()) {
      if (i++ % 2 == 0) {
        retired.push_back(id);
        deletes.push_back(op_delete(id));
      }
    }
    const commit_receipt d = must(tg::commit_ops(m, editor, deletes));
    std::map<record_id, record_id> tombstones; // id -> retirer
    for (const record_id &id : retired) {
      CHECK(retirer(m, id) == d.tx.id());
      tombstones.emplace(id, d.tx.id());
    }

    // Random edits, each over the live records only; random meta (undo_of naming random txs).
    tg::oracle_store live = must(tg::oracle_of(must(m.head())));
    for (int step = 0; step < 6; ++step) {
      patch p = must(gen.next(live));
      p.base.reset();
      must(tg::commit_ops(m, editor, p.ops, p.meta));
      live = must(tg::oracle_of(must(m.head())));
    }
    const snapshot before = must(m.head());
    for (const auto &[id, by] : tombstones) {
      CHECK(must(lifetime_of(before, id)) == id_lifetime::tombstoned);
      CHECK(must(tombstone_of(before, id)).value_or(tombstone{}).retired_by == by);
    }

    // Every revival not naming the retirer is refused and writes nothing.
    const tx_id stranger = must(tx_id::from(gen.values().random_id(id_class::tx)));
    for (const record_id &id : retired) {
      const patch_op create = op_create(id, gen.type(), gen.body());
      for (const patch_meta &meta : {patch_meta{}, undo_of({load.tx}), undo_of({stranger})}) {
        transaction t = attempt(m, editor, {create}, meta);
        CHECK(t.state() == tx_state::rejected);
        CHECK(reports_reuse(t.diagnostics(), id));
        ++refused;
      }
    }
    CHECK(must(m.head()).revision_digest() == before.revision_digest());

    // undo_of naming the retirer revives them all, in one transaction.
    std::vector<patch_op> revive;
    revive.reserve(retired.size());
    for (const record_id &id : retired) {
      revive.push_back(op_create(id, oracle.records().find(id)->second.type,
                                 oracle.records().find(id)->second.body));
    }
    must(tg::commit_ops(m, editor, revive, undo_of({stranger, d.tx})));
    for (const record_id &id : retired) {
      CHECK(life(m, id) == id_lifetime::live);
      CHECK(must(must(m.head()).get(id)).body == oracle.records().find(id)->second.body);
      ++revived;
    }
  }
  CHECK(refused == revived * 3);
  CHECK(revived >= seeds * 3);
}
