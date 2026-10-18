// FAB-INV-COR-03 (SL-ARCH-002 ARCH2-R41): the loss report equals the oracle exactly and surviving
// records verify. Two heads planted on top of the sample (an undecodable record e1, then a valid
// change to e2) are both lost by a lenient open: the report names exactly those two revisions,
// exactly e1 and e2 and their documents, and is exact; every record of the recovered view reads
// and verifies and equals the record of the last good head. compute_loss() over nothing recovered
// lists every record of the lost head; a lost revision whose root is missing is reported but not
// exact; no lost revision is no loss.

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/corrupt_store.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <set>
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

template <class T> std::vector<T> sorted(const std::set<T> &s) { return {s.begin(), s.end()}; }

} // namespace

TEST_CASE("FAB-INV-COR-03: the loss of a lenient view equals the oracle and the surviving "
          "records verify") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const record_id e1 = tg::entity_named("e1");
  const record_id e2 = tg::entity_named("e2");
  record_object r1;
  record_object r2;
  std::vector<snapshot_entry> good;
  {
    auto ws = must(tg::open_sample(s, cfg));
    r1 = must(tg::record_at(ws->store(), s.heads.back(), e1));
    r2 = must(tg::record_at(ws->store(), s.heads.back(), e2));
    const snapshot head = must(snapshot::open(ws->store(), s.heads.back()));
    must_ok(head.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
      good.push_back(e);
      return {};
    }));
  }
  const std::vector<std::uint8_t> garbage = {0xff, 0x00};
  const digest lost1 =
      must(tg::plant_record(s, e1, make_object(object_kind::record, garbage), cfg));
  r2.body = must(tg::create_entity("e2 changed")).new_value.value_or(value{});
  const digest lost2 = must(tg::plant_record(s, e2, must(encode_record_object(r2)), cfg));

  const checked_open view = must(open_checked(s.root, corruption_mode::lenient, recovery_policy{},
                                              must(tg::sample_options(s.fs, cfg))));
  REQUIRE(view.recovered_view);
  REQUIRE(view.recovered_revision.has_value());
  const digest rev = view.recovered_revision.value_or(digest{});
  CHECK(rev == s.heads.back());

  // The oracle.
  CHECK(view.loss.revisions == sorted(std::set<digest>{lost1, lost2}));
  CHECK(view.loss.records == sorted(std::set<record_id>{e1, e2}));
  std::set<record_id> docs;
  for (const record_object &r : {r1, r2}) {
    if (!r.document.is_null()) {
      docs.insert(r.document);
    }
  }
  CHECK(view.loss.documents == sorted(docs));
  CHECK(view.loss.exact);

  // The surviving records verify and equal the last good head.
  const workspace &ws = *view.ws;
  CHECK(must(state_closure_verifies(ws.store(), rev)));
  const snapshot at = must(ws.at(rev));
  std::size_t n = 0;
  must_ok(at.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
    ++n;
    const object o = must(ws.store().get(e.object));
    CHECK(o.id == e.object);
    const auto it = std::ranges::find(good, e.id, &snapshot_entry::id);
    REQUIRE(it != good.end());
    CHECK(it->object == e.object);
    CHECK(it->body == e.body);
    return {};
  }));
  CHECK(n == good.size());
  CHECK(must(ws.store().verify(verify_scope{})).ok());
}

TEST_CASE("FAB-INV-COR-03: compute_loss over nothing recovered, a missing root and no loss") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  auto ws = must(tg::open_sample(s, cfg));
  const store_backend &store = ws->store();

  const std::vector<digest> head = {s.heads.back()};
  const loss_report all = must(compute_loss(store, std::nullopt, head));
  std::set<record_id> live;
  std::set<record_id> docs;
  const snapshot snap = must(snapshot::open(store, s.heads.back()));
  must_ok(snap.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
    live.insert(e.id);
    if (!e.document.is_null()) {
      docs.insert(e.document);
    }
    return {};
  }));
  CHECK(all.records == sorted(live));
  CHECK(all.documents == sorted(docs));
  CHECK(all.revisions == head);
  CHECK(all.exact);

  // The last commit's loss against the one before: what it changed (e1 renamed, e<n> created).
  const std::vector<digest> last = {s.heads.back()};
  const loss_report one = must(compute_loss(store, s.heads[s.heads.size() - 2], last));
  CHECK(one.records ==
        sorted(std::set<record_id>{tg::entity_named("e1"),
                                   tg::entity_named("e" + std::to_string(s.heads.size()))}));
  CHECK(one.exact);

  digest absent;
  absent.bytes.fill(0x42);
  const std::vector<digest> unknown = {absent};
  const loss_report blind = must(compute_loss(store, s.heads.back(), unknown));
  CHECK(blind.revisions == unknown);
  CHECK(blind.records.empty());
  CHECK_FALSE(blind.exact);

  const loss_report none = must(compute_loss(store, s.heads.back(), {}));
  CHECK(none == loss_report{});
}
