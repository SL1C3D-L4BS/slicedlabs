// FAB-INV-TX-08 (SL-ARCH-002 ARCH2-R10, ARCH2-R01; ADR-0016): history is append-only and abort
// leaves no trace. Every commit adds one revision whose parent is the previous head and older
// revisions keep their roots; ref creation and reset are recorded transactions (reset keeps the
// old head as a parent); an aborted or destroyed transaction, over seeded patch_gen patches on
// both backends, leaves objects, refs, sequence and events untouched. The F5 barrier is checked at
// compile time: a commit needs a mutation_capability, which nothing here can make, and neither
// the manager, the transaction nor the workspace exposes another way to write a batch.

#include "sl/fabric/events.hpp"
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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

// ---- F5 at compile time (ARCH2-R01): the only door is the capability ----
// Each concept asks whether an expression compiles for its (dependent) type: substitution
// failure, access checks included, makes it false.
template <class S>
concept commit_without_capability = requires(S &s, commit_batch b) { s.commit(std::move(b)); };
template <class S>
concept commit_with_braced_capability =
    requires(S &s, commit_batch b) { s.commit({}, std::move(b)); };
template <class C>
concept capability_constructible = requires { C{}; };
template <class M>
concept manager_writes_batches = requires(M &m, commit_batch b) { m.write_batch(std::move(b)); };
template <class M>
concept manager_commits_batches = requires(M &m, commit_batch b) { m.commit(std::move(b)); };
template <class T>
concept transaction_commits_batches = requires(T &t, commit_batch b) { t.commit(std::move(b)); };
template <class S>
concept const_store_commits = requires(const S &s, commit_batch b) {
  s.commit(std::declval<const mutation_capability &>(), std::move(b));
};

// Positive controls: the same shapes compile where they should, so the negatives are not vacuous.
template <class S>
concept store_commits_with_capability = requires(S &s, commit_batch b) {
  s.commit(std::declval<const mutation_capability &>(), std::move(b));
};
template <class M>
concept manager_begins = requires(M &m, tx_options o) { m.begin(o); };
template <class T>
concept transaction_commits = requires(T &t) { t.commit(); };
static_assert(store_commits_with_capability<store_backend>);
static_assert(manager_begins<transaction_manager>);
static_assert(transaction_commits<transaction>);
static_assert(capability_constructible<commit_batch>);

static_assert(!capability_constructible<mutation_capability>);
static_assert(!std::is_default_constructible_v<mutation_capability>);
static_assert(!std::is_copy_constructible_v<mutation_capability>);
static_assert(!std::is_move_constructible_v<mutation_capability>);
static_assert(!std::is_copy_assignable_v<mutation_capability>);
static_assert(!std::is_move_assignable_v<mutation_capability>);
static_assert(!commit_without_capability<store_backend>);
static_assert(!commit_with_braced_capability<store_backend>);
static_assert(!manager_writes_batches<transaction_manager>); // C4 is private to the manager
static_assert(!manager_commits_batches<transaction_manager>);
static_assert(!transaction_commits_batches<transaction>);
static_assert(!const_store_commits<store_backend>);
static_assert(!std::is_default_constructible_v<transaction_manager>);
static_assert(!std::is_copy_constructible_v<transaction>);
// A workspace hands out its store read-only; the manager too.
static_assert(requires(workspace &w) {
  { w.store() } -> std::same_as<const store_backend &>;
});
static_assert(requires(transaction_manager &m) {
  { m.store() } -> std::same_as<const store_backend &>;
});

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

gen::fabric_history::revision revision_of(const store_backend &s, const digest &id) {
  return must(decode_revision(must(s.get(id))));
}

gen::fabric_history::tx_record tx_of(const store_backend &s, const digest &revision) {
  return must(decode_tx_record(must(s.get(revision_of(s, revision).tx))));
}

} // namespace

TEST_CASE("FAB-INV-TX-08: history is append-only; refs are created and reset by recorded "
          "transactions") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");

  std::vector<commit_receipt> commits;
  commits.push_back(must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))})));
  for (int i = 0; i < 4; ++i) {
    commits.push_back(
        must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "v" + std::to_string(i)))})));
  }
  for (std::size_t i = 0; i < commits.size(); ++i) {
    const auto rev = revision_of(*store, commits[i].revision);
    CHECK(rev.generation == i);
    CHECK(rev.root == commits[i].root);
    if (i == 0) {
      CHECK(rev.parents.empty());
    } else {
      CHECK(rev.parents == std::vector<digest>{commits[i - 1].revision});
      CHECK(commits[i].seq == commits[i - 1].seq + 1);
    }
    // Old revisions stay readable with their own state.
    CHECK(must(m.at(commits[i].revision)).root_digest() == commits[i].root);
  }

  // create_ref: a revision over the target with the same root, recorded in history.
  const commit_receipt created = must(m.create_ref("feature", commits[1].revision, editor));
  CHECK(created.root == commits[1].root);
  CHECK(revision_of(*store, created.revision).parents == std::vector<digest>{commits[1].revision});
  CHECK(tx_of(*store, created.revision).label == "create_ref feature");
  CHECK(must(store->read_ref("feature")) == created.revision);
  CHECK(m.create_ref("feature", commits[2].revision, editor).error().code == errc::already_exists);

  // reset_ref: main goes back to commit 1's state; the old head stays reachable as a parent.
  const commit_receipt reset = must(m.reset_ref("main", commits[1].revision, editor));
  const auto reset_rev = revision_of(*store, reset.revision);
  CHECK(reset.root == commits[1].root);
  CHECK(reset_rev.parents == std::vector<digest>{commits[1].revision, commits.back().revision});
  CHECK(tx_of(*store, reset.revision).label == "reset_ref main");
  CHECK(must(m.head()).root_digest() == commits[1].root);
  CHECK(m.reset_ref("nowhere", commits[1].revision, editor).error().code == errc::not_found);
  // Nothing was rewritten: every earlier revision is still there.
  for (const commit_receipt &c : commits) {
    CHECK(must(store->has(c.revision)));
  }
}

TEST_CASE("FAB-INV-TX-08: abort leaves no trace (seeded patches, both backends)") {
  for (const bool durable : {false, true}) {
    CAPTURE(durable);
    tg::tx_log_store log;
    auto memory = std::make_shared<memory_backend>();
    std::shared_ptr<store_backend> store = durable ? must(log.open()) : memory;
    auto ws = must(
        workspace::create_with(store, must(tg::gen_options(std::make_shared<manual_clock>(1)))));
    transaction_manager &m = ws->manager();
    std::uint64_t events = 0;
    auto sub = m.events().subscribe("", [&events](const event_envelope &) noexcept { ++events; });
    tg::patch_gen_options opts;
    opts.meta_percent = 0;
    tg::patch_gen gen(7, opts);
    tg::oracle_store oracle = must(gen.state(5));
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor());
    auto load = must(m.begin(o));
    must_ok(load.stage(tg::load_patch(oracle)));
    must(load.commit());

    const auto image = durable ? must(tg::file_image(log.memory(), "/ws"))
                               : std::map<std::string, std::vector<std::uint8_t>>{};
    const auto objects = memory->object_count();
    const auto refs = must(store->list_refs());
    const std::uint64_t events_before = events;
    for (int i = 0; i < 12; ++i) {
      CAPTURE(i);
      patch p = must(gen.next(oracle));
      p.base.reset();
      auto t = must(m.begin(o));
      must_ok(t.stage(p));
      if (i % 2 == 0) {
        t.abort();
        CHECK(t.state() == tx_state::aborted);
        CHECK(t.commit().error().code == errc::tx_aborted);
        CHECK(t.stage(p).error().code == errc::tx_closed);
      } // odd: destroyed while open
    }
    CHECK(must(store->list_refs()) == refs);
    CHECK(memory->object_count() == objects);
    CHECK(events == events_before);
    CHECK(must(tg::oracle_of(must(m.head()))) == oracle);
    if (durable) {
      CHECK(must(tg::file_image(log.memory(), "/ws")) == image);
    }
  }
}
