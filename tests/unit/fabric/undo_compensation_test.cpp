// FAB-INV-UNDO-08 (SL-ARCH-002 ARCH2-R24; ADR-0018): a compensation runs as its own transaction
// inside intent_begin and intent_end. An entry that declares a compensation (op_compensation) is
// undone by the inverse of its reversible ops, then by the registered compensator's patch committed
// as its own transaction, with the intent records written just before and just after that commit
// (store sequence numbers prove the bracket), outcome compensated. The compensation transaction
// carries the undo_of of its undo; the compensated undo cannot be redone. A failing compensator
// ends the intent abandoned; an unregistered one refuses before anything is written.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

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

// "test.unpublish": creates the entity named after the string argument.
std::shared_ptr<tg::function_compensator> unpublisher() {
  return std::make_shared<tg::function_compensator>(
      "test.unpublish", [](const compensation &c, const snapshot &) -> sl::result<patch> {
        auto op = tg::create_entity(c.args.as_string());
        if (!op) {
          return std::unexpected(op.error());
        }
        patch p;
        p.ops.push_back(std::move(*op));
        return p;
      });
}

struct bench {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  std::unique_ptr<workspace> ws;
  std::shared_ptr<tg::seq_intent_log> intents;
  std::shared_ptr<compensator_registry> compensators = std::make_shared<compensator_registry>();
};

bench make_bench() {
  bench b;
  b.ws = must(
      workspace::create_with(b.store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  b.intents = std::make_shared<tg::seq_intent_log>(*b.store);
  return b;
}

// hero exists; the editor's group renames it and declares a compensation "test.unpublish".
tx_id publish(transaction_manager &m, undo_manager &um, std::string_view retraction) {
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(um.begin_group("publish"));
  const commit_receipt r = must(
      tg::commit_ops(m, editor,
                     {must(tg::set_name(hero, "published")),
                      must(op_compensation(compensation{.command = "test.unpublish",
                                                        .args = must(value::string(retraction))}))},
                     um.group_meta()));
  must_ok(um.end_group());
  return r.tx;
}

} // namespace

TEST_CASE("FAB-INV-UNDO-08: a compensation runs as its own transaction inside intent_begin and "
          "intent_end") {
  bench b = make_bench();
  transaction_manager &m = b.ws->manager();
  auto comp = unpublisher();
  must_ok(b.compensators->add(comp));
  CHECK(b.compensators->add(unpublisher()).error().code == errc::already_exists);
  undo_manager um(
      m, tg::tx_editor(),
      undo_options{.ref = "main", .intents = b.intents, .compensators = b.compensators});
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {must(tg::create_entity("hero"))}));
  const std::string original(must(must(m.head()).get(hero)).body.find_field(1)->as_string());
  const tx_id published = publish(m, um, "retraction");
  CHECK(um.undo_stack().back().compensations == 1);

  const std::uint64_t seq0 = b.store->last_seq();
  const commit_receipt r = must(um.undo());
  CHECK(comp->calls() == 1);
  // Two transactions: the inverse (seq0 + 1), then the compensation (seq0 + 2).
  CHECK(b.store->last_seq() == seq0 + 2);
  CHECK(r.seq == seq0 + 2);
  const auto log = b.intents->records();
  REQUIRE(log.size() == 2);
  CHECK(log[0].record.phase == intent_phase::begin);
  CHECK(log[0].record.kind == "compensate:test.unpublish");
  CHECK(log[0].seq == seq0 + 1); // after the inverse, before the compensation commit
  CHECK(log[1].record.phase == intent_phase::end);
  CHECK(log[1].record.outcome == intent_outcome::compensated);
  CHECK(log[1].seq == seq0 + 2); // right after the compensation commit
  CHECK(log[0].record.operation == log[1].record.operation);
  CHECK(log[0].record.operation.cls() == id_class::operation);

  const snapshot head = must(m.head());
  CHECK(must(head.get(hero)).body.find_field(1)->as_string() == original);
  CHECK(must(head.contains(tg::entity_named("retraction"))));
  const patch cp = must(tg::stored_patch(*b.store, r.revision));
  CHECK(cp.meta.undo_of == std::vector<tx_id>{published});
  CHECK(cp.meta.idempotency_key.starts_with("sl.undo.compensate/"));
  CHECK(must(tg::stored_tx(*b.store, r.revision)).undo_of == published.id());
  // A compensated undo cannot be redone; the stacks stay a function of history.
  CHECK_FALSE(um.can_redo());
  CHECK(um.undo_stack().size() == 1);
  undo_manager rebuilt(m, tg::tx_editor());
  CHECK(rebuilt.undo_stack() == um.undo_stack());
  CHECK(rebuilt.redo_stack() == um.redo_stack());
}

TEST_CASE("FAB-INV-UNDO-08: a failing compensator abandons its intent; a missing one writes "
          "nothing") {
  bench b = make_bench();
  transaction_manager &m = b.ws->manager();
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {must(tg::create_entity("hero"))}));
  {
    // No compensator registered: refused before anything is written.
    undo_manager um(m, tg::tx_editor(),
                    undo_options{.ref = "main", .intents = b.intents, .compensators = nullptr});
    publish(m, um, "retraction");
    const auto objects = b.store->object_count();
    const auto head = must(m.head()).revision_digest();
    CHECK(um.undo().error().code == errc::not_found);
    CHECK(b.store->object_count() == objects);
    CHECK(must(m.head()).revision_digest() == head);
    CHECK(b.intents->records().empty());
    CHECK(um.can_undo());
  }
  // The compensator fails: the inverse is committed, the intent ends abandoned.
  must_ok(b.compensators->add(std::make_shared<tg::function_compensator>(
      "test.unpublish", [](const compensation &, const snapshot &) -> sl::result<patch> {
        return sl::make_error(errc::store_io, "the remote refused");
      })));
  undo_manager um(
      m, tg::tx_editor(),
      undo_options{.ref = "main", .intents = b.intents, .compensators = b.compensators});
  const std::uint64_t seq0 = b.store->last_seq();
  auto failed = um.undo();
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().code == errc::store_io);
  CHECK(b.store->last_seq() == seq0 + 1);
  const auto log = b.intents->records();
  REQUIRE(log.size() == 2);
  CHECK(log[0].record.phase == intent_phase::begin);
  CHECK(log[1].record.phase == intent_phase::end);
  CHECK(log[1].record.outcome == intent_outcome::abandoned);

  // The default intent log is in memory.
  memory_intent_log mem;
  must_ok(mem.write(intent_record{.operation = tg::entity_named("op"),
                                  .kind = "k",
                                  .phase = intent_phase::begin,
                                  .outcome = intent_outcome::none}));
  CHECK(mem.open() == std::vector<record_id>{tg::entity_named("op")});
  CHECK(mem.write(intent_record{}).error().code == errc::invalid_argument);
  // A compensation note round-trips; other ops declare none.
  const patch_op op = must(op_compensation(compensation{.command = "c", .args = value::uint(7)}));
  CHECK(must(compensation_of(op)) == compensation{.command = "c", .args = value::uint(7)});
  CHECK_FALSE(must(compensation_of(op_annotate(value::uint(1)))).has_value());
  CHECK(op_compensation(compensation{}).error().code == errc::invalid_argument);
}
