// FAB-INV-TX-16 (SL-ARCH-002 ARCH2-R13; master plan section 3, F6): committed, rejected and
// conflicted events carry correlation and arrive in seq order. Each envelope validates, carries
// the origin's correlation (or the thread's correlation_scope when the origin gives none), names
// the tx and revision in its causality for a commit and the ref scope for a refusal, and its
// payload decodes to the tx record. Events are delivered after the commit point only, after the
// commit observers (step C6), whose attachments reach the receipt.

#include "sl/core/log.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
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

// C6: observers run before the events; each attaches a summary to the receipt.
class counting_observer final : public commit_observer {
public:
  explicit counting_observer(const event_bus &bus) : bus_(&bus) {}
  [[nodiscard]] sl::result<void> on_commit(const commit_event &event,
                                           receipt_builder &receipt) override {
    delivered_at_commit_.push_back(bus_->delivered());
    seqs_.push_back(event.seq);
    return receipt.attach("test.changed", value::uint(event.changed.size()));
  }
  [[nodiscard]] const std::vector<std::uint64_t> &delivered_at_commit() const noexcept {
    return delivered_at_commit_;
  }
  [[nodiscard]] const std::vector<std::uint64_t> &seqs() const noexcept { return seqs_; }

private:
  const event_bus *bus_;
  std::vector<std::uint64_t> delivered_at_commit_;
  std::vector<std::uint64_t> seqs_;
};

} // namespace

TEST_CASE("FAB-INV-TX-16: committed, rejected and conflicted events carry correlation and arrive "
          "in seq order") {
  auto ws =
      must(workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(42)))));
  transaction_manager &m = ws->manager();
  auto observer = std::make_shared<counting_observer>(m.events());
  m.add_observer(observer);
  std::vector<event_envelope> seen;
  auto sub = m.events().subscribe("fabric.tx.",
                                  [&seen](const event_envelope &e) noexcept { seen.push_back(e); });

  tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  editor.correlation = correlation_record{.id = 77, .scope = "tests.tx_events", .tick = 3};
  const commit_receipt first = must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const record_id hero = tg::entity_named("hero");

  // Rejected: the agent may not write.
  tx_origin agent = tg::tx_origin_of(tg::tx_agent());
  agent.correlation = correlation_record{.id = 78, .scope = "tests.tx_events", .tick = 4};
  CHECK(tg::commit_ops(m, agent, {must(tg::set_name(hero, "agent"))}).error().code ==
        errc::validation_failed);

  // Conflicted: a stale base under reject_stale. No correlation on the origin: the thread's.
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_editor());
  auto late = must(m.begin(o));
  must_ok(late.stage(must(tg::set_name(hero, "late"))));
  const commit_receipt second = must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "v2"))}));
  {
    const sl::log::correlation_scope scope(
        sl::log::correlation{.id = 79, .scope = "tests.thread", .tick = 5});
    CHECK(late.commit().error().code == errc::stale_base);
  }

  REQUIRE(seen.size() == 4);
  // The observer saw both commits, each before its committed event was delivered.
  CHECK(observer->seqs() == std::vector<std::uint64_t>{first.seq, second.seq});
  CHECK(observer->delivered_at_commit() == std::vector<std::uint64_t>{0, 2});
  REQUIRE(first.attachments.contains("test.changed"));
  CHECK(first.attachments.find("test.changed")->second == value::uint(1));
  const std::vector<std::string> types = {"fabric.tx.committed", "fabric.tx.rejected",
                                          "fabric.tx.committed", "fabric.tx.conflicted"};
  for (std::size_t i = 0; i < seen.size(); ++i) {
    CAPTURE(i);
    CHECK(seen[i].type == types[i]);
    CHECK(validate(seen[i]).has_value());
    CHECK(seen[i].seq == i + 1); // in seq order, one stream
    REQUIRE(seen[i].payload.has_value());
    const auto tx = must(gen::fabric_history::tx_record::from_value(
        must(decode(seen[i].payload.value_or(std::vector<std::uint8_t>{})))));
    CHECK(tx.actor == seen[i].actor);
  }
  CHECK(seen[0].correlation.id == 77);
  CHECK(seen[0].correlation.scope == "tests.tx_events");
  CHECK(seen[0].correlation.tick == 3);
  CHECK(seen[1].correlation.id == 78);
  CHECK(seen[3].correlation.id == 79);
  CHECK(seen[3].correlation.scope == "tests.thread");

  // Committed: about the tx, caused by it, at its revision.
  CHECK(std::get<record_id>(seen[0].about.alt) == first.tx.id());
  CHECK(seen[0].causality.tx == first.tx.id());
  CHECK(seen[0].causality.revision == first.revision);
  CHECK(seen[2].causality.revision == second.revision);
  CHECK(seen[0].id == event_id_for(first.tx.id(), "fabric.tx.committed", 1));
  // Refusals: about the ref, at the base they were staged over.
  CHECK(std::get<std::string>(seen[1].about.alt) == "fabric.tx/main");
  CHECK(seen[1].causality.revision == first.revision);
  CHECK(std::get<std::string>(seen[3].about.alt) == "fabric.tx/main");
  CHECK(seen[3].causality.revision == first.revision);
  CHECK(seen[1].actor == tg::tx_agent().id());
  CHECK(m.events().last_seq() == 4);
}
