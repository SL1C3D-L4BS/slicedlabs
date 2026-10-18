// FAB-INV-STO-02 (SL-ARCH-002 ARCH2-R02): putting an existing object is idempotent: committing an
// object that is already stored (in a later batch, or twice in one batch) succeeds, stores one
// copy, leaves its bytes unchanged and adds no write step that could change them.

#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include "../../../engine/fabric/testing/store_faults.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using sl::fabric::commit_batch;
using sl::fabric::memory_backend;
using sl::fabric::object;
using sl::fabric::object_kind;
using sl::fabric::testing::backdoor;

object text_object(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(object_kind::blob, bytes);
}

commit_batch objects(std::vector<object> list) {
  commit_batch b;
  b.objects = std::move(list);
  return b;
}

} // namespace

TEST_CASE("FAB-INV-STO-02: committing a stored object again is idempotent") {
  memory_backend store;
  const object a = text_object("alpha");
  const object b = text_object("beta");
  REQUIRE(backdoor::commit(store, objects({a})).has_value());
  CHECK(store.object_count() == 1);

  for (int round = 0; round < 3; ++round) {
    const sl::result<sl::fabric::commit_ack> ack = backdoor::commit(store, objects({a}));
    REQUIRE(ack.has_value());
    CHECK(store.object_count() == 1);
    CHECK(store.get(a.id).value_or(object{}) == a);
  }
  REQUIRE(backdoor::commit(store, objects({a, b, a, b})).has_value());
  CHECK(store.object_count() == 2);
  CHECK(store.get(a.id).value_or(object{}) == a);
  CHECK(store.get(b.id).value_or(object{}) == b);
  CHECK(store.last_seq() == 5);
  CHECK(store.verify({}).value_or(sl::fabric::verify_report{}).ok());
}

TEST_CASE("FAB-INV-STO-02: a failed batch never removes an object stored before it") {
  const auto faults = std::make_shared<sl::fabric::testing::store_faults>();
  memory_backend store(faults);
  const object a = text_object("alpha");
  const object b = text_object("beta");
  REQUIRE(backdoor::commit(store, objects({a})).has_value());
  // The batch re-puts a, writes b, then fails at its last step: the rollback must remove b only.
  faults->fail_after(2); // object 0 (a, already stored), object 1 (b), seq fails
  const sl::result<sl::fabric::commit_ack> failed = backdoor::commit(store, objects({a, b}));
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().code == sl::errc::store_io);
  CHECK(store.object_count() == 1);
  CHECK(store.get(a.id).value_or(object{}) == a);
  CHECK_FALSE(store.has(b.id).value_or(true));
}
