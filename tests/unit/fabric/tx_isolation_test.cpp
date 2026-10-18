// FAB-INV-TX-02 (SL-ARCH-002 ARCH2-R12, ARCH2-R02): staged changes are invisible to every other
// snapshot and transaction until commit; a snapshot taken before a commit never changes after it,
// and an aborted transaction was never visible at all.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

std::string name_in(const state_reader &s, const record_id &id) {
  const auto r = must(s.read(id));
  if (!r) {
    return "<absent>";
  }
  return std::string(must(r->body.get({path_segment::field(1)})).as_string());
}

} // namespace

TEST_CASE("FAB-INV-TX-02: staged changes are invisible to other snapshots and transactions until "
          "commit") {
  auto ws = must(
      workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(5'000'000)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const record_id hero = tg::entity_named("hero");

  const snapshot before = must(m.head());
  tx_options o;
  o.origin = editor;
  auto writer = must(m.begin(o));
  must_ok(writer.stage(must(tg::set_name(hero, "staged"))));
  must_ok(writer.stage(must(tg::create_entity("sidekick"))));

  // The writer reads its own writes; nobody else does.
  CHECK(name_in(writer.view(), hero) == "staged");
  CHECK(name_in(writer.view(), tg::entity_named("sidekick")) == "sidekick");
  CHECK(name_in(must(m.head()), hero) == "hero");
  CHECK(name_in(must(m.head()), tg::entity_named("sidekick")) == "<absent>");
  auto reader = must(m.begin(o));
  CHECK(name_in(reader.view(), hero) == "hero");
  CHECK(must(m.head()).revision_digest() == before.revision_digest());

  const commit_receipt r = must(writer.commit());
  CHECK(name_in(must(m.head()), hero) == "staged");
  // Snapshots are immutable: the one taken before still reads the old state.
  CHECK(name_in(before, hero) == "hero");
  CHECK(name_in(before, tg::entity_named("sidekick")) == "<absent>");
  CHECK(before.revision_digest() != r.revision);
  // A transaction begun before the commit keeps its own base.
  CHECK(name_in(reader.view(), hero) == "hero");
  CHECK(reader.base() == before.revision_digest());
  reader.abort();
}

TEST_CASE("FAB-INV-TX-02: an aborted transaction and a failed one were never visible") {
  auto ws = must(
      workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(5'000'000)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const snapshot before = must(m.head());
  {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(tg::entity_named("hero"), "ghost"))));
    // destroyed open: aborted
  }
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_agent()); // denied by authority
  auto denied = must(m.begin(o));
  must_ok(denied.stage(must(tg::set_name(tg::entity_named("hero"), "ghost"))));
  CHECK_FALSE(denied.commit().has_value());
  CHECK(must(m.head()).revision_digest() == before.revision_digest());
  CHECK(name_in(must(m.head()), tg::entity_named("hero")) == "hero");
}
