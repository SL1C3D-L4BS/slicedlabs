// FAB-INV-STO-04 (SL-ARCH-002 ARCH2-R02): ref compare-and-swap: an expected mismatch is
// ref_cas_failed and writes nothing; without an expected value the ref is created only if absent.

#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::commit_batch;
using sl::fabric::digest;
using sl::fabric::memory_backend;
using sl::fabric::object;
using sl::fabric::object_kind;
using sl::fabric::ref_update;
using sl::fabric::testing::backdoor;

object text_object(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(object_kind::revision, bytes);
}

commit_batch cas(const object &o, std::string name, std::optional<digest> expected) {
  commit_batch b;
  b.objects.push_back(o);
  b.refs.push_back(ref_update{.name = std::move(name), .expected = expected, .desired = o.id});
  return b;
}

errc code_of(const sl::result<sl::fabric::commit_ack> &r) { return r ? errc::ok : r.error().code; }

} // namespace

TEST_CASE("FAB-INV-STO-04: create-if-absent creates a missing ref and refuses an existing one") {
  memory_backend store;
  const object r1 = text_object("revision 1");
  const object r2 = text_object("revision 2");
  CHECK(code_of(backdoor::commit(store, cas(r1, "main", std::nullopt))) == errc::ok);
  CHECK(store.read_ref("main").value_or(digest{}) == r1.id);

  const sl::result<sl::fabric::commit_ack> again =
      backdoor::commit(store, cas(r2, "main", std::nullopt));
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == errc::ref_cas_failed);
  CHECK(again.error().message.find("create-if-absent") != std::string::npos);
  CHECK(store.read_ref("main").value_or(digest{}) == r1.id);
  CHECK_FALSE(store.has(r2.id).value_or(true));
  CHECK(store.last_seq() == 1);
}

TEST_CASE("FAB-INV-STO-04: an expected mismatch rejects and an exact match moves the ref") {
  memory_backend store;
  const object r1 = text_object("revision 1");
  const object r2 = text_object("revision 2");
  const object r3 = text_object("revision 3");
  REQUIRE(backdoor::commit(store, cas(r1, "main", std::nullopt)).has_value());

  // Stale expectation: the head is r1, the writer believes r2.
  CHECK(code_of(backdoor::commit(store, cas(r3, "main", r2.id))) == errc::ref_cas_failed);
  CHECK(store.read_ref("main").value_or(digest{}) == r1.id);
  CHECK_FALSE(store.has(r3.id).value_or(true));
  // An expectation on a ref that does not exist.
  CHECK(code_of(backdoor::commit(store, cas(r3, "feature", r1.id))) == errc::ref_cas_failed);
  CHECK_FALSE(store.read_ref("feature").has_value());

  CHECK(code_of(backdoor::commit(store, cas(r2, "main", r1.id))) == errc::ok);
  CHECK(store.read_ref("main").value_or(digest{}) == r2.id);
  // The old expectation is now stale too.
  CHECK(code_of(backdoor::commit(store, cas(r3, "main", r1.id))) == errc::ref_cas_failed);
  CHECK(code_of(backdoor::commit(store, cas(r3, "main", r2.id))) == errc::ok);
  CHECK(store.read_ref("main").value_or(digest{}) == r3.id);
  CHECK(store.last_seq() == 3);
}

TEST_CASE("FAB-INV-STO-04: two writers from the same base: exactly one CAS wins") {
  memory_backend store;
  const object base = text_object("base");
  const object left = text_object("left");
  const object right = text_object("right");
  REQUIRE(backdoor::commit(store, cas(base, "main", std::nullopt)).has_value());
  CHECK(code_of(backdoor::commit(store, cas(left, "main", base.id))) == errc::ok);
  CHECK(code_of(backdoor::commit(store, cas(right, "main", base.id))) == errc::ref_cas_failed);
  CHECK(store.read_ref("main").value_or(digest{}) == left.id);
}

TEST_CASE("FAB-INV-STO-04: a ref never points at an object the store lacks") {
  memory_backend store;
  const object r1 = text_object("revision 1");
  commit_batch dangling;
  dangling.refs.push_back(ref_update{.name = "main", .expected = std::nullopt, .desired = r1.id});
  CHECK(code_of(backdoor::commit(store, dangling)) == errc::object_missing);
  CHECK(store.read_ref("main").error().code == errc::not_found);
  REQUIRE(backdoor::commit(store, cas(r1, "pin", std::nullopt)).has_value());
  CHECK(code_of(backdoor::commit(store, dangling)) == errc::ok); // stored now
  CHECK(store.read_ref("main").value_or(digest{}) == r1.id);
}
