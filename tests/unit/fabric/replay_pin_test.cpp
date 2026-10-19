// FAB-INV-RPL-02 (SL-ARCH-002 ARCH2-R51, ARCH2-R10; master plan, Volume 04, replay seams): a replay
// pins the revision it replays with a ref, and a ref keeps the revision and its whole closure
// reachable under a sweep. The sweep here is the reachability walk the store's verify uses
// (revision_links from every ref); the swept store keeps only what it reaches. A pinned
// revision on a side branch survives the sweep, opens and verifies with every record intact;
// the same sweep without the pin ref loses it (negative control).

#include "sl/fabric/clock.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

constexpr std::string_view pin_ref = "replay/pin-001";

// A store holding only the closure of the refs of `from` (minus `drop`), with those refs.
std::shared_ptr<memory_backend> sweep(const store_backend &from, std::string_view drop) {
  const revision_links links;
  std::vector<ref_entry> kept;
  for (const ref_entry &r : must(from.list_refs())) {
    if (r.name != drop) {
      kept.push_back(r);
    }
  }
  std::set<digest> seen;
  std::vector<digest> todo;
  todo.reserve(kept.size());
  for (const ref_entry &r : kept) {
    todo.push_back(r.target);
  }
  std::vector<object> objects;
  while (!todo.empty()) {
    const digest id = todo.back();
    todo.pop_back();
    if (!seen.insert(id).second) {
      continue;
    }
    object o = must(from.get(id));
    std::vector<digest> next;
    REQUIRE(links.append_links(o, next).has_value());
    todo.insert(todo.end(), next.begin(), next.end());
    objects.push_back(std::move(o));
  }
  auto out = std::make_shared<memory_backend>();
  commit_batch batch{.objects = std::move(objects), .refs = {}, .intents = {}, .commit_time_us = 1};
  for (const ref_entry &r : kept) {
    batch.refs.push_back(ref_update{.name = r.name, .expected = std::nullopt, .desired = r.target});
  }
  REQUIRE(testing::backdoor::commit(*out, std::move(batch)).has_value());
  return out;
}

std::vector<snapshot_entry> entries(const snapshot &s) {
  std::vector<snapshot_entry> out;
  REQUIRE(s.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
             out.push_back(e);
             return {};
           }).has_value());
  return out;
}

struct pinned {
  std::unique_ptr<workspace> ws;
  digest revision;
  std::vector<snapshot_entry> records;
};

// main: lamp, rope, then rope renamed. The pin branches from the first revision and renames lamp,
// so its revision and its record object are reachable from the pin ref only.
pinned make_pinned() {
  auto clock = std::make_shared<manual_clock>(1'000'000);
  pinned p;
  p.ws = must(workspace::create_memory(must(tg::scene_options(clock, 7))));
  transaction_manager &m = p.ws->manager();
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  const commit_receipt first = must(tg::commit_ops(m, origin, {must(tg::create_entity("lamp"))}));
  (void)must(tg::commit_ops(m, origin, {must(tg::create_entity("rope"))}));
  (void)must(tg::commit_ops(m, origin, {must(tg::set_name(tg::entity_named("rope"), "cable"))}));
  (void)must(m.create_ref(pin_ref, first.revision, origin));
  const commit_receipt side = must(tg::commit_ops(
      m, origin, {must(tg::set_name(tg::entity_named("lamp"), "pinned lamp"))}, {}, pin_ref));
  p.revision = side.revision;
  p.records = entries(must(m.at(side.revision)));
  return p;
}

} // namespace

TEST_CASE("FAB-INV-RPL-02: a pin ref keeps the pinned revision reachable under sweep") {
  const pinned p = make_pinned();
  REQUIRE(p.records.size() == 1);
  const store_backend &store = p.ws->manager().store();
  auto swept = sweep(store, "");
  CHECK(must(swept->has(p.revision)));
  CHECK(must(swept->read_ref(pin_ref)) == p.revision);

  const revision_links links;
  const verify_report report =
      must(swept->verify(verify_scope{.hashes = true, .reachability = true, .links = &links}));
  CHECK(report.ok());
  CHECK(report.refs_walked == 2);

  auto clock = std::make_shared<manual_clock>(2'000'000);
  auto reopened = must(workspace::create_with(swept, must(tg::scene_options(clock, 7))));
  const snapshot at = must(reopened->manager().at(p.revision));
  CHECK(entries(at) == p.records);
  CHECK(at.revision_digest() == std::optional<digest>(p.revision));
}

TEST_CASE("FAB-INV-RPL-02: without the pin ref the sweep loses the revision") {
  const pinned p = make_pinned();
  auto swept = sweep(p.ws->manager().store(), pin_ref);
  CHECK_FALSE(must(swept->has(p.revision)));
  CHECK_FALSE(swept->read_ref(pin_ref).has_value());
  auto clock = std::make_shared<manual_clock>(2'000'000);
  auto reopened = must(workspace::create_with(swept, must(tg::scene_options(clock, 7))));
  CHECK_FALSE(reopened->manager().at(p.revision).has_value());
  // main survives either way
  CHECK(must(swept->read_ref("main")) == must(p.ws->manager().store().read_ref("main")));
}
