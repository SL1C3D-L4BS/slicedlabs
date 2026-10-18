// FAB-INV-STO-03 (SL-ARCH-002 ARCH2-R07): a commit batch is all or nothing under an injected
// failure at each step. A batch of objects (one already stored), a ref move, a ref creation and
// two intent records is failed at every internal step in turn; each time the store must equal its
// state before the batch, and the batch must then still commit whole with the next seq.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include "../../../engine/fabric/testing/store_faults.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::fabric::commit_batch;
using sl::fabric::digest;
using sl::fabric::intent_outcome;
using sl::fabric::intent_phase;
using sl::fabric::intent_record;
using sl::fabric::memory_backend;
using sl::fabric::object;
using sl::fabric::object_kind;
using sl::fabric::record_id;
using sl::fabric::ref_entry;
using sl::fabric::ref_update;
using sl::fabric::testing::backdoor;
using sl::fabric::testing::store_faults;

object text_object(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(object_kind::blob, bytes);
}

record_id op(std::uint32_t n) { return sl::fabric::well_known(sl::fabric::id_class::tx, n); }

// Everything a client can observe about the store.
struct snapshot {
  std::size_t objects = 0;
  std::vector<bool> present;
  std::vector<ref_entry> refs;
  std::vector<intent_record> intents;
  std::vector<record_id> open;
  std::uint64_t seq = 0;

  friend bool operator==(const snapshot &, const snapshot &) = default;
};

snapshot observe(const memory_backend &s) {
  const object a = text_object("alpha");
  const object b = text_object("beta");
  const object c = text_object("gamma");
  snapshot out;
  out.objects = s.object_count();
  for (const object &o : {a, b, c}) {
    out.present.push_back(s.has(o.id).value_or(false));
    if (out.present.back()) {
      REQUIRE(s.get(o.id).value_or(object{}) == o);
    }
  }
  out.refs = s.list_refs().value_or(std::vector<ref_entry>{});
  out.intents = s.intents();
  out.open = s.open_intents();
  out.seq = s.last_seq();
  return out;
}

void seed(memory_backend &s) {
  const object a = text_object("alpha");
  commit_batch base;
  base.objects = {a};
  base.refs = {ref_update{.name = "main", .expected = std::nullopt, .desired = a.id},
               ref_update{.name = "side", .expected = std::nullopt, .desired = a.id}};
  base.intents = {intent_record{.operation = op(1),
                                .kind = "compensate",
                                .phase = intent_phase::begin,
                                .outcome = intent_outcome::none}};
  REQUIRE(backdoor::commit(s, std::move(base)).has_value());
}

commit_batch the_batch() {
  const object a = text_object("alpha");
  const object b = text_object("beta");
  const object c = text_object("gamma");
  commit_batch batch;
  batch.objects = {a, b, c};
  batch.refs = {ref_update{.name = "main", .expected = a.id, .desired = b.id},
                ref_update{.name = "feature", .expected = std::nullopt, .desired = c.id}};
  batch.intents = {intent_record{.operation = op(1),
                                 .kind = "compensate",
                                 .phase = intent_phase::end,
                                 .outcome = intent_outcome::completed},
                   intent_record{.operation = op(2),
                                 .kind = "import",
                                 .phase = intent_phase::begin,
                                 .outcome = intent_outcome::none}};
  batch.commit_time_us = 42;
  return batch;
}

} // namespace

TEST_CASE("FAB-INV-STO-03: the commit steps are traced in order") {
  const auto faults = std::make_shared<store_faults>();
  memory_backend store(faults);
  seed(store);
  const std::uint64_t before = faults->steps();
  REQUIRE(backdoor::commit(store, the_batch()).has_value());
  const std::vector<std::string> trace = faults->trace();
  REQUIRE(trace.size() >= before);
  const std::vector<std::string> steps(trace.begin() + static_cast<std::ptrdiff_t>(before),
                                       trace.end());
  CHECK(steps == std::vector<std::string>{"object 0", "object 1", "object 2", "ref main",
                                          "ref feature", "intent 0", "intent 1", "seq"});
}

TEST_CASE("FAB-INV-STO-03: a batch failed at any step leaves the store unchanged") {
  const object a = text_object("alpha");
  const object b = text_object("beta");
  const object c = text_object("gamma");
  std::uint64_t total = 0;
  {
    const auto faults = std::make_shared<store_faults>();
    memory_backend store(faults);
    seed(store);
    const std::uint64_t before = faults->steps();
    REQUIRE(backdoor::commit(store, the_batch()).has_value());
    total = faults->steps() - before;
  }
  REQUIRE(total == 8);

  for (std::uint64_t k = 0; k < total; ++k) {
    CAPTURE(k);
    const auto faults = std::make_shared<store_faults>();
    memory_backend store(faults);
    seed(store);
    const snapshot baseline = observe(store);
    faults->fail_after(k);
    const sl::result<sl::fabric::commit_ack> failed = backdoor::commit(store, the_batch());
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::store_io);
    CHECK(faults->injected() == 1);
    CHECK(observe(store) == baseline);
    CHECK(store.verify({}).value_or(sl::fabric::verify_report{.findings = {{}}}).ok());

    // The same batch then commits whole, with the next sequence number.
    const sl::result<sl::fabric::commit_ack> ok = backdoor::commit(store, the_batch());
    REQUIRE(ok.has_value());
    CHECK(ok->seq == baseline.seq + 1);
    const snapshot after = observe(store);
    CHECK(after.objects == 3);
    CHECK(after.refs == std::vector<ref_entry>{{.name = "feature", .target = c.id},
                                               {.name = "main", .target = b.id},
                                               {.name = "side", .target = a.id}});
    CHECK(after.intents.size() == 3);
    CHECK(after.open == std::vector<record_id>{op(2)});
  }
}

TEST_CASE("FAB-INV-STO-03: a failure on the first commit of an empty store leaves it empty") {
  const object a = text_object("alpha");
  const object b = text_object("beta");
  for (std::uint64_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    const auto faults = std::make_shared<store_faults>();
    memory_backend store(faults);
    faults->fail_after(k);
    commit_batch batch;
    batch.objects = {a, b};
    batch.refs = {ref_update{.name = "main", .expected = std::nullopt, .desired = b.id}};
    REQUIRE_FALSE(backdoor::commit(store, std::move(batch)).has_value());
    CHECK(store.object_count() == 0);
    CHECK(store.list_refs().value_or(std::vector<ref_entry>{{}}).empty());
    CHECK(store.last_seq() == 0);
  }
}
