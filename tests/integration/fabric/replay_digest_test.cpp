// FAB-INV-RPL-01 (SL-ARCH-002 ARCH2-R50, ARCH2-R11; master plan, Volume 04, replay seams): with an
// injected clock and an injected id source, replaying the recorded transactions of a workload
// reproduces the same revision object ids and roots. The workload mints ids through the manager,
// creates, edits, deletes and undoes; the replay reads every first-parent revision of the recorded
// history (revision -> tx record -> patch object, nothing kept on the side), sets the clock to the
// recorded commit time and commits the recorded patch with the recorded origin into a fresh
// workspace seeded alike. A second recording with the same seed mints the same ids and reaches
// the same head; another seed does not (negative control).

#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

struct recording {
  std::unique_ptr<workspace> ws;
  std::shared_ptr<manual_clock> clock;
  std::vector<record_id> minted;
  std::map<std::string, std::int64_t> commit_time; // revision hex -> commit time
  digest head;
};

constexpr std::int64_t start_us = 1'760'000'000'000'000;

patch_meta labeled(std::string label) {
  patch_meta meta;
  meta.label = std::move(label);
  return meta;
}

recording record(std::uint64_t seed) {
  recording r;
  r.clock = std::make_shared<manual_clock>(start_us);
  r.ws = must(workspace::create_memory(must(tg::scene_options(r.clock, seed))));
  transaction_manager &m = r.ws->manager();
  undo_manager undo(m, tg::tx_editor());
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  auto note = [&](const commit_receipt &c) {
    r.commit_time[c.revision.to_hex()] = c.commit_time_us;
    r.clock->advance_us(1'250);
  };
  for (int i = 0; i < 3; ++i) {
    const record_id id = must(m.mint(id_class::entity));
    r.minted.push_back(id);
    note(must(tg::commit_ops(m, origin,
                             {op_create(id, tg::entity_type(), must(tg::sample_entity("lantern")))},
                             labeled("create minted"))));
  }
  note(must(tg::commit_ops(m, origin, {must(tg::create_entity("rope"))})));
  note(must(tg::commit_ops(m, origin, {must(tg::set_name(r.minted[0], "lamp post"))},
                           labeled("rename"))));
  note(must(tg::commit_ops(m, origin, {op_delete(r.minted[2])}, labeled("delete"))));
  note(must(undo.undo()));
  const std::optional<digest> head = must(m.head()).revision_digest();
  REQUIRE(head.has_value());
  r.head = head.value_or(digest{});
  return r;
}

struct replay_step {
  digest revision;
  digest root;
  gen::fabric_history::tx_record tx;
  patch p;
};

// The first-parent history of `head`, oldest first, read from the store alone.
std::vector<replay_step> history(const store_backend &store, digest head) {
  std::vector<replay_step> out;
  std::optional<digest> cur = head;
  while (cur) {
    const auto rev = must(decode_revision(must(store.get(*cur))));
    auto tx = must(decode_tx_record(must(store.get(rev.tx))));
    const object po = must(store.get(tx.patch));
    REQUIRE(po.header.kind == object_kind::patch);
    patch p = must(from_value(must(decode(po.payload))));
    out.push_back({.revision = *cur, .root = rev.root, .tx = std::move(tx), .p = std::move(p)});
    cur = rev.parents.empty() ? std::nullopt : std::optional<digest>(rev.parents.front());
  }
  std::ranges::reverse(out);
  return out;
}

} // namespace

TEST_CASE("FAB-INV-RPL-01: replaying recorded transactions reproduces revision ids and roots") {
  const recording rec = record(62);
  const auto steps = history(rec.ws->manager().store(), rec.head);
  REQUIRE(steps.size() == rec.commit_time.size());

  auto clock = std::make_shared<manual_clock>(start_us);
  auto ws = must(workspace::create_memory(must(tg::scene_options(clock, 62))));
  transaction_manager &m = ws->manager();
  for (const replay_step &s : steps) {
    clock->set_us(rec.commit_time.at(s.revision.to_hex()));
    tx_origin origin{.actor = actor_id::from(s.tx.actor).value_or(actor_id{}),
                     .subsystem = s.tx.origin.subsystem,
                     .correlation = std::nullopt,
                     .provenance = s.tx.provenance,
                     .epistemic = s.tx.origin.epistemic,
                     .class_changes = {}};
    tx_options options;
    options.origin = std::move(origin);
    auto tx = must(m.begin(std::move(options)));
    REQUIRE(tx.stage(s.p).has_value());
    const commit_receipt c = must(tx.commit());
    CHECK_MESSAGE(c.revision == s.revision, s.tx.label);
    CHECK(c.root == s.root);
    CHECK(c.commit_time_us == rec.commit_time.at(s.revision.to_hex()));
  }
  CHECK(must(m.head()).revision_digest() == std::optional<digest>(rec.head));
}

TEST_CASE("FAB-INV-RPL-01: the same seed mints the same ids and reaches the same head") {
  const recording a = record(62);
  const recording b = record(62);
  CHECK(a.minted == b.minted);
  CHECK(a.head == b.head);
  const recording other = record(63);
  CHECK(other.minted != a.minted);
  CHECK(other.head != a.head);
}
