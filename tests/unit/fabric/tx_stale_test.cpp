// FAB-INV-TX-03 (SL-ARCH-002 ARCH2-R08): a commit whose base moved is `stale_base` under
// reject_stale (the default), with the head unchanged, the transaction in Conflict and a report
// naming the base, the head and the intervening revisions; the same holds on the log backend.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

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

void stale_on(const std::shared_ptr<store_backend> &store) {
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const commit_receipt base =
      must(tg::commit_ops(m, editor, {must(tg::create_entity("a")), must(tg::create_entity("b"))}));

  tx_options o;
  o.origin = editor;
  auto late = must(m.begin(o));
  // Disjoint from the intervening commit: reject_stale refuses anyway.
  must_ok(late.stage(must(tg::set_name(tg::entity_named("b"), "b2"))));
  const commit_receipt first =
      must(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("a"), "a2"))}));
  const std::uint64_t seq = first.seq;

  const auto r = late.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::stale_base);
  CHECK(late.state() == tx_state::conflict);
  CHECK(must(m.head()).revision_digest() == first.revision);
  CHECK(must(store->read_ref("main")) == first.revision);
  REQUIRE(late.conflict().has_value());
  const conflict_report report = late.conflict().value_or(conflict_report{});
  CHECK(report.code == errc::stale_base);
  CHECK(report.ref == "main");
  CHECK(report.base == base.revision);
  CHECK(report.head == first.revision);
  CHECK(report.intervening == std::vector<digest>{first.revision});
  CHECK(report.overlaps.empty());
  CHECK_FALSE(report.ours.entries.empty());
  // Nothing was written: the next commit takes the next sequence number.
  const commit_receipt next =
      must(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("b"), "b3"))}));
  CHECK(next.seq == seq + 1);
  // A closed transaction stays closed.
  CHECK(late.commit().error().code == errc::tx_closed);
  CHECK(late.stage(must(tg::set_name(tg::entity_named("b"), "x"))).error().code == errc::tx_closed);
}

} // namespace

TEST_CASE("FAB-INV-TX-03: a stale base under reject_stale is stale_base with the head unchanged "
          "(memory_backend)") {
  stale_on(std::make_shared<memory_backend>());
}

TEST_CASE("FAB-INV-TX-03: a stale base under reject_stale is stale_base with the head unchanged "
          "(log_backend)") {
  tg::tx_log_store log;
  stale_on(must(log.open()));
}
