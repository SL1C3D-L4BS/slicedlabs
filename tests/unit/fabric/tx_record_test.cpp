// FAB-INV-TX-15 (SL-ARCH-002 ARCH2-R16, ARCH2-R04; SL-ARCH-001 ARCH1-R45): one commit makes
// exactly one revision whose tx record decodes to the committed patch: one more store sequence
// number, one more generation, the revision's tx record names the actor, label, origin and key,
// its patch object decodes to a normalised patch based on the parent whose ops are the staged ops
// (coalesced), and the receipt's tx id is the one derived from the tx record. Seeded patch_gen
// workloads on both backends: after every commit the head equals the oracle_store, and the stored
// patch replayed over the parent snapshot gives the committed root.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

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

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

struct stored_commit {
  gen::fabric_history::revision revision;
  gen::fabric_history::tx_record tx;
  digest tx_digest;
  patch patch_value;
};

stored_commit read_commit(const store_backend &s, const digest &revision) {
  stored_commit out;
  out.revision = must(decode_revision(must(s.get(revision))));
  const object txo = must(s.get(out.revision.tx));
  out.tx_digest = txo.id;
  out.tx = must(decode_tx_record(txo));
  const object po = must(s.get(out.tx.patch));
  REQUIRE(po.header.kind == object_kind::patch);
  out.patch_value = must(from_value(must(decode(po.payload))));
  return out;
}

patch strip(patch p) {
  p.base.reset();
  for (patch_op &op : p.ops) {
    op.expect = expectation{};
    op.captured.reset();
    op.expect_order_key.clear();
    op.expect_type = type_id{};
    if (op.kind == op_kind::delete_record) {
      op.type = type_id{};
    }
  }
  return p;
}

} // namespace

TEST_CASE("FAB-INV-TX-15: one commit makes exactly one revision whose tx record decodes to the "
          "committed patch") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(11)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const commit_receipt base = must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const record_id hero = tg::entity_named("hero");

  patch_meta meta;
  meta.label = "rename hero";
  meta.idempotency_key = "rename-1";
  tx_options o;
  o.origin = editor;
  o.meta = meta;
  auto t = must(m.begin(o));
  must_ok(t.stage(must(tg::set_name(hero, "first"))));
  must_ok(t.stage(must(tg::set_name(hero, "second")))); // coalesces with the first
  must_ok(t.stage(must(tg::create_entity("sidekick"))));
  const std::size_t objects_before = store->object_count();
  const commit_receipt r = must(t.commit());

  CHECK(r.seq == base.seq + 1);
  CHECK(store->last_seq() == r.seq);
  const stored_commit c = read_commit(*store, r.revision);
  CHECK(c.revision.parents == std::vector<digest>{base.revision});
  CHECK(c.revision.generation == 1);
  CHECK(c.revision.root == r.root);
  CHECK(c.tx.actor == tg::tx_editor().id());
  CHECK(c.tx.label == "rename hero");
  CHECK(c.tx.idempotency_key == std::optional<std::string>("rename-1"));
  CHECK(c.tx.origin.subsystem == "05");
  CHECK(r.tx.id() == derive(id_class::tx, tg::tx_editor().id(), "tx/" + c.tx_digest.to_hex()));
  CHECK(r.undo.tx == r.tx);
  CHECK(r.undo.tx_record == c.tx_digest);
  CHECK(r.undo.revision == r.revision);
  CHECK(r.impact.total == 2); // hero and sidekick

  const patch &p = c.patch_value;
  CHECK(p.base == base.revision);
  CHECK(is_normalized(p));
  CHECK(p.meta.label == meta.label);
  CHECK(p.meta.idempotency_key == meta.idempotency_key);
  REQUIRE(p.ops.size() == 2);
  patch want;
  want.ops = {must(tg::set_name(hero, "second"))};
  CHECK(strip(p).ops[0] == strip(want).ops[0]);
  CHECK(p.ops[0].expect == expectation::of(must(value::string("hero"))));
  CHECK(p.ops[1].kind == op_kind::create_record);
  // One revision only: the objects written are the new ones of this single commit.
  CHECK(store->object_count() > objects_before);
  CHECK(must(m.head()).generation() == 1);
}

TEST_CASE("FAB-INV-TX-15: seeded workloads: the head equals the oracle after every commit and "
          "the stored patch replays to the committed root (both backends)") {
  for (std::uint64_t seed = 11; seed <= 13; ++seed) {
    for (const bool durable : {false, true}) {
      CAPTURE(seed);
      CAPTURE(durable);
      tg::tx_log_store log;
      std::shared_ptr<store_backend> store =
          durable ? must(log.open()) : std::make_shared<memory_backend>();
      auto ws = must(workspace::create_with(
          store, must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
      transaction_manager &m = ws->manager();
      tg::patch_gen_options opts;
      opts.meta_percent = 0;
      tg::patch_gen gen(seed, opts);
      tg::oracle_store oracle = must(gen.state(8));
      tx_options o;
      o.origin = tg::tx_origin_of(tg::tx_editor());
      auto load = must(m.begin(o));
      must_ok(load.stage(tg::load_patch(oracle)));
      must(load.commit());
      for (int step = 0; step < 12; ++step) {
        CAPTURE(step);
        patch p = must(gen.next(oracle));
        p.base.reset();
        const snapshot parent = must(m.head());
        auto t = must(m.begin(o));
        must_ok(t.stage(p));
        const commit_receipt r = must(t.commit());
        must_ok(oracle.apply(p));
        const snapshot head = must(m.head());
        CHECK(head.revision_digest() == r.revision);
        CHECK(must(tg::oracle_of(head)) == oracle);
        // Replay: the stored patch over the parent gives the same records.
        const stored_commit c = read_commit(*store, r.revision);
        CHECK(c.revision.parents ==
              std::vector<digest>{parent.revision_digest().value_or(digest{})});
        staged_state replay(parent);
        must_ok(replay.apply(c.patch_value));
        tg::oracle_store replayed = must(tg::oracle_of(parent));
        for (const staged_change &ch : replay.changes()) {
          if (ch.after) {
            replayed.put(ch.id, *ch.after);
          } else {
            replayed.erase(ch.id);
          }
        }
        CHECK(replayed == oracle);
      }
    }
  }
}
