// FAB-INV-TX-06 (SL-ARCH-002 ARCH2-R09): a retried commit with the same (actor, idempotency key)
// and the same patch returns the original receipt and writes nothing; another patch under that key
// is `idempotency_mismatch`; keys are per actor; after a reopen the index is rebuilt from the tx
// records (memory and log backends), so a retry still returns the original revision and a
// mismatch is still refused.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

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

patch_meta keyed(std::string key) {
  patch_meta m;
  m.label = "keyed";
  m.idempotency_key = std::move(key);
  return m;
}

std::unique_ptr<workspace> open(const std::shared_ptr<store_backend> &store,
                                std::int64_t now = 10) {
  return must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(now)))));
}

std::optional<digest> head_of(const store_backend &s) {
  auto r = s.read_ref("main");
  return r ? std::optional<digest>(*r) : std::nullopt;
}

void idempotent_on(const std::shared_ptr<store_backend> &store) {
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  auto ws = open(store);
  transaction_manager &m = ws->manager();
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const commit_receipt original =
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "one"))}, keyed("k1")));
  const std::optional<digest> head = head_of(*store);

  // The same patch under the same key: the original receipt, nothing written.
  const commit_receipt retry =
      must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "one"))}, keyed("k1")));
  CHECK(retry == original);
  CHECK(head_of(*store) == head);
  CHECK(m.commits() == 2);

  // Another patch under that key: refused, nothing written.
  tx_options o;
  o.origin = editor;
  o.meta = keyed("k1");
  auto other = must(m.begin(o));
  REQUIRE(other.stage(must(tg::set_name(hero, "two"))).has_value());
  const auto refused = other.commit();
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::idempotency_mismatch);
  CHECK(other.state() == tx_state::rejected);
  CHECK(head_of(*store) == head);

  // Keys are per actor: the same key from another actor is a new commit.
  const commit_receipt theirs = must(tg::commit_ops(
      m, tg::tx_origin_of(tg::tx_other()), {must(tg::set_name(hero, "two"))}, keyed("k1")));
  CHECK(theirs.revision != original.revision);
  const std::optional<digest> final_head = head_of(*store);
  ws.reset();

  // After a reopen the index comes back from the tx records.
  auto again = open(store, 99);
  const commit_receipt recovered = must(
      tg::commit_ops(again->manager(), editor, {must(tg::set_name(hero, "one"))}, keyed("k1")));
  CHECK(recovered.recovered);
  CHECK(recovered.revision == original.revision);
  CHECK(recovered.root == original.root);
  CHECK(recovered.tx == original.tx);
  CHECK(recovered.undo == original.undo);
  CHECK(head_of(*store) == final_head);
  const auto mismatch =
      tg::commit_ops(again->manager(), editor, {must(tg::set_name(hero, "three"))}, keyed("k1"));
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().code == errc::idempotency_mismatch);
  CHECK(head_of(*store) == final_head);
}

} // namespace

TEST_CASE("FAB-INV-TX-06: a keyed retry returns the original receipt; another patch is "
          "idempotency_mismatch (memory_backend)") {
  idempotent_on(std::make_shared<memory_backend>());
}

TEST_CASE("FAB-INV-TX-06: a keyed retry returns the original receipt; another patch is "
          "idempotency_mismatch (log_backend)") {
  tg::tx_log_store log;
  idempotent_on(must(log.open()));
}

TEST_CASE("FAB-INV-TX-06: expectations and coalescing do not change what counts as the same "
          "patch") {
  auto ws = open(std::make_shared<memory_backend>());
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  // Two sets of one path coalesce into one op (ADR-0018). Another value under the key is refused
  // whatever the base; the coalesced form with an author-filled expectation is the same patch.
  const commit_receipt original = must(tg::commit_ops(
      m, editor, {must(tg::set_name(hero, "x")), must(tg::set_name(hero, "y"))}, keyed("drag")));
  tx_options o;
  o.origin = editor;
  o.base = must(m.at(original.revision)).revision_digest(); // any base: the key decides first
  o.meta = keyed("drag");
  auto t = must(m.begin(o));
  REQUIRE(t.stage(must(tg::set_name(hero, "z"))).has_value());
  const auto mismatch = t.commit();
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().code == errc::idempotency_mismatch);
  o.base.reset();
  auto again = must(m.begin(o));
  REQUIRE(again
              .stage(op_set(hero, {path_segment::field(1)}, must(value::string("y")),
                            expectation::of(must(value::string("y")))))
              .has_value());
  const commit_receipt same = must(again.commit());
  CHECK(same == original);
}
