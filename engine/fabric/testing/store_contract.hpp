#pragma once

// The store_backend contract suite (WO-042, SL-ARCH-002 ARCH2-R02, FAB-INV-STO-05; WO-053
// instantiates it for log_backend as FAB-INV-STO-06). Header-only doctest code, included by test
// files only (as "../../../engine/fabric/testing/store_contract.hpp" from tests/unit/fabric):
//
//   struct my_factory { std::unique_ptr<sl::fabric::store_backend> make(); };
//   TEST_CASE("FAB-INV-STO-nn: the backend contract suite passes on my_backend") {
//     my_factory f;
//     sl::fabric::testing::store_contract(f);
//   }
//
// Each clause is a doctest SUBCASE on a fresh backend from the factory; mutations go through
// testing::backdoor, the only capability holder outside transaction_manager.

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"

#include <doctest/doctest.h>

namespace sl::fabric::testing {

/// Makes a fresh, empty backend per call.
template <class F>
concept store_factory = requires(F &f) {
  { f.make() } -> std::convertible_to<std::unique_ptr<store_backend>>;
};

namespace contract_detail {

inline object blob(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return make_object(object_kind::blob, bytes);
}

/// A tree object whose payload is the 32 hash bytes of each child (all SHA-256 ids).
inline object tree(const std::vector<digest> &children) {
  std::vector<std::uint8_t> payload;
  for (const digest &d : children) {
    payload.insert(payload.end(), d.bytes.begin(), d.bytes.end());
  }
  return make_object(object_kind::tree, payload);
}

/// Reads the links of tree() objects; every other kind has none.
class tree_links final : public link_reader {
public:
  [[nodiscard]] result<void> append_links(const object &obj,
                                          std::vector<digest> &out) const noexcept override {
    if (obj.header.kind != object_kind::tree) {
      return {};
    }
    if (obj.payload.size() % sha256::digest_size != 0) {
      return make_error(errc::object_corrupt, "tree payload is not a list of digests");
    }
    for (std::size_t at = 0; at < obj.payload.size(); at += sha256::digest_size) {
      digest d;
      for (std::size_t i = 0; i < sha256::digest_size; ++i) {
        d.bytes[i] = obj.payload[at + i];
      }
      out.push_back(d);
    }
    return {};
  }
};

inline record_id operation(std::uint32_t ordinal) { return well_known(id_class::tx, ordinal); }

inline commit_batch objects_batch(std::vector<object> objects) {
  return commit_batch{
      .objects = std::move(objects), .refs = {}, .intents = {}, .commit_time_us = 1};
}

inline commit_batch ref_batch(std::vector<object> objects, std::string name,
                              std::optional<digest> expected, digest desired) {
  commit_batch b = objects_batch(std::move(objects));
  b.refs.push_back(ref_update{.name = std::move(name), .expected = expected, .desired = desired});
  return b;
}

inline bool stored(const store_backend &s, const digest &id) {
  const result<bool> h = s.has(id);
  REQUIRE(h.has_value());
  return *h;
}

inline errc code_of(const result<commit_ack> &r) { return r ? errc::ok : r.error().code; }

} // namespace contract_detail

template <store_factory Factory> void store_contract(Factory &factory) {
  using namespace contract_detail;
  const std::unique_ptr<store_backend> store_ptr = factory.make();
  REQUIRE(store_ptr != nullptr);
  store_backend &store = *store_ptr;
  const object a = blob("alpha");
  const object b = blob("beta");
  const object c = blob("gamma");

  SUBCASE("an empty store holds nothing") {
    CHECK_FALSE(stored(store, a.id));
    const result<object> got = store.get(a.id);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == errc::object_missing);
    const result<digest> ref = store.read_ref("main");
    REQUIRE_FALSE(ref.has_value());
    CHECK(ref.error().code == errc::not_found);
    const result<std::vector<ref_entry>> refs = store.list_refs();
    REQUIRE(refs.has_value());
    CHECK(refs->empty());
  }

  SUBCASE("a committed object reads back verified and equal") {
    const result<commit_ack> ack = backdoor::commit(store, objects_batch({a, b}));
    REQUIRE(ack.has_value());
    CHECK(ack->seq == 1);
    for (const object &o : {a, b}) {
      CHECK(stored(store, o.id));
      const result<object> got = store.get(o.id);
      REQUIRE(got.has_value());
      CHECK(*got == o);
      CHECK(hash_object(got->header, got->payload) == o.id);
      CHECK_FALSE(got->read_only());
    }
    CHECK_FALSE(stored(store, c.id));
  }

  SUBCASE("a ref is created if absent, then moved by compare-and-swap") {
    REQUIRE(backdoor::commit(store, ref_batch({a}, "main", std::nullopt, a.id)).has_value());
    const result<digest> head = store.read_ref("main");
    REQUIRE(head.has_value());
    CHECK(*head == a.id);
    REQUIRE(backdoor::commit(store, ref_batch({b}, "main", a.id, b.id)).has_value());
    CHECK(store.read_ref("main").value_or(digest{}) == b.id);
    CHECK(stored(store, a.id)); // history stays
  }

  SUBCASE("putting an existing object is idempotent") {
    REQUIRE(backdoor::commit(store, objects_batch({a})).has_value());
    const result<commit_ack> again = backdoor::commit(store, objects_batch({a, a, b}));
    REQUIRE(again.has_value());
    CHECK(again->seq == 2);
    CHECK(store.get(a.id).value_or(object{}) == a);
    CHECK(store.get(b.id).value_or(object{}) == b);
    CHECK(store.verify(verify_scope{}).value_or(verify_report{}).objects_hashed == 2);
  }

  SUBCASE("an expected mismatch fails the CAS and writes nothing") {
    REQUIRE(backdoor::commit(store, ref_batch({a}, "main", std::nullopt, a.id)).has_value());
    const result<commit_ack> bad = backdoor::commit(store, ref_batch({c}, "main", b.id, c.id));
    CHECK(code_of(bad) == errc::ref_cas_failed);
    CHECK_FALSE(stored(store, c.id));
    CHECK(store.read_ref("main").value_or(digest{}) == a.id);
    const result<commit_ack> missing = backdoor::commit(store, ref_batch({c}, "other", a.id, c.id));
    CHECK(code_of(missing) == errc::ref_cas_failed);
    CHECK_FALSE(store.read_ref("other").has_value());
  }

  SUBCASE("create-if-absent fails on an existing ref") {
    REQUIRE(backdoor::commit(store, ref_batch({a}, "main", std::nullopt, a.id)).has_value());
    const result<commit_ack> bad =
        backdoor::commit(store, ref_batch({b}, "main", std::nullopt, b.id));
    CHECK(code_of(bad) == errc::ref_cas_failed);
    CHECK_FALSE(stored(store, b.id));
    CHECK(store.read_ref("main").value_or(digest{}) == a.id);
  }

  SUBCASE("one failing ref refuses the whole batch") {
    REQUIRE(backdoor::commit(store, ref_batch({a}, "main", std::nullopt, a.id)).has_value());
    commit_batch batch = ref_batch({b, c}, "side", std::nullopt, b.id);
    batch.refs.push_back(ref_update{.name = "main", .expected = c.id, .desired = c.id});
    CHECK(code_of(backdoor::commit(store, std::move(batch))) == errc::ref_cas_failed);
    CHECK_FALSE(stored(store, b.id));
    CHECK_FALSE(stored(store, c.id));
    CHECK_FALSE(store.read_ref("side").has_value());
  }

  SUBCASE("a ref target must be stored or in the batch") {
    const result<commit_ack> bad =
        backdoor::commit(store, ref_batch({}, "main", std::nullopt, a.id));
    CHECK(code_of(bad) == errc::object_missing);
    CHECK_FALSE(store.read_ref("main").has_value());
    REQUIRE(backdoor::commit(store, objects_batch({a})).has_value());
    CHECK(backdoor::commit(store, ref_batch({}, "main", std::nullopt, a.id)).has_value());
  }

  SUBCASE("an object whose id is not its hash is refused") {
    object forged = a;
    forged.id = b.id;
    CHECK(code_of(backdoor::commit(store, objects_batch({c, forged}))) == errc::object_corrupt);
    CHECK_FALSE(stored(store, b.id));
    CHECK_FALSE(stored(store, c.id));
  }

  SUBCASE("frames of a newer version are refused for writing") {
    object newer_minor = a;
    newer_minor.header.minor = 1;
    newer_minor.id = hash_object(newer_minor.header, newer_minor.payload);
    CHECK(newer_minor.read_only());
    CHECK(code_of(backdoor::commit(store, objects_batch({newer_minor}))) == errc::read_only_newer);
    object newer_major = a;
    newer_major.header.major = 2;
    newer_major.id = hash_object(newer_major.header, newer_major.payload);
    CHECK(code_of(backdoor::commit(store, objects_batch({newer_major}))) ==
          errc::format_unsupported);
    object flagged = a;
    flagged.header.flags = 1;
    flagged.id = hash_object(flagged.header, flagged.payload);
    CHECK(code_of(backdoor::commit(store, objects_batch({flagged}))) == errc::object_corrupt);
    CHECK_FALSE(stored(store, newer_minor.id));
  }

  SUBCASE("malformed batches are invalid_argument and write nothing") {
    CHECK(code_of(backdoor::commit(store, commit_batch{})) == errc::invalid_argument);
    CHECK(code_of(backdoor::commit(store, ref_batch({a}, "", std::nullopt, a.id))) ==
          errc::invalid_argument);
    CHECK(code_of(backdoor::commit(store, ref_batch({a}, "bad name", std::nullopt, a.id))) ==
          errc::invalid_argument);
    CHECK(code_of(backdoor::commit(store, ref_batch({a}, std::string(256, 'r'), std::nullopt,
                                                    a.id))) == errc::invalid_argument);
    commit_batch twice = ref_batch({a, b}, "main", std::nullopt, a.id);
    twice.refs.push_back(ref_update{.name = "main", .expected = std::nullopt, .desired = b.id});
    CHECK(code_of(backdoor::commit(store, std::move(twice))) == errc::invalid_argument);
    CHECK_FALSE(stored(store, a.id));
    CHECK(backdoor::commit(store, ref_batch({a}, std::string(255, 'r'), std::nullopt, a.id))
              .has_value());
  }

  SUBCASE("sequence numbers count successful commits only") {
    CHECK(backdoor::commit(store, objects_batch({a})).value_or(commit_ack{}).seq == 1);
    CHECK_FALSE(backdoor::commit(store, commit_batch{}).has_value());
    CHECK_FALSE(backdoor::commit(store, ref_batch({}, "x", std::nullopt, c.id)).has_value());
    CHECK(backdoor::commit(store, objects_batch({b})).value_or(commit_ack{}).seq == 2);
    CHECK(backdoor::commit(store, objects_batch({a})).value_or(commit_ack{}).seq == 3);
  }

  SUBCASE("refs are listed sorted by name") {
    for (const std::string_view name : {"zeta", "main", "pins/b", "alpha", "pins/a"}) {
      REQUIRE(backdoor::commit(store, ref_batch({a}, std::string(name), std::nullopt, a.id))
                  .has_value());
    }
    const result<std::vector<ref_entry>> refs = store.list_refs();
    REQUIRE(refs.has_value());
    std::vector<std::string> names;
    for (const ref_entry &r : *refs) {
      names.push_back(r.name);
      CHECK(r.target == a.id);
    }
    CHECK(names == std::vector<std::string>{"alpha", "main", "pins/a", "pins/b", "zeta"});
  }

  SUBCASE("intents bracket an operation") {
    const intent_record begin{.operation = operation(1),
                              .kind = "compensate",
                              .phase = intent_phase::begin,
                              .outcome = intent_outcome::none};
    const intent_record end{.operation = operation(1),
                            .kind = "compensate",
                            .phase = intent_phase::end,
                            .outcome = intent_outcome::completed};
    commit_batch open_batch;
    open_batch.intents.push_back(begin);
    CHECK(backdoor::commit(store, open_batch).has_value());
    CHECK(code_of(backdoor::commit(store, open_batch)) == errc::invalid_argument); // open twice
    commit_batch close_batch;
    close_batch.intents.push_back(end);
    CHECK(backdoor::commit(store, close_batch).has_value());
    CHECK(code_of(backdoor::commit(store, close_batch)) == errc::invalid_argument); // not open
    intent_record bad_end = end;
    bad_end.outcome = intent_outcome::none;
    commit_batch bad;
    bad.intents.push_back(bad_end);
    CHECK(code_of(backdoor::commit(store, bad)) == errc::invalid_argument);
    intent_record null_op = begin;
    null_op.operation = record_id{};
    bad.intents = {null_op};
    CHECK(code_of(backdoor::commit(store, bad)) == errc::invalid_argument);
  }

  SUBCASE("verify on a healthy store finds nothing") {
    const object root = tree({a.id, b.id});
    REQUIRE(backdoor::commit(store, ref_batch({a, b, c, root}, "main", std::nullopt, root.id))
                .has_value());
    const tree_links links;
    const result<verify_report> report =
        store.verify(verify_scope{.hashes = true, .reachability = true, .links = &links});
    REQUIRE(report.has_value());
    CHECK(report->ok());
    CHECK(report->objects_hashed == 4);
    CHECK(report->objects_reached == 3);
    CHECK(report->refs_walked == 1);
  }

  SUBCASE("verify reachability names a missing linked object") {
    const object root = tree({a.id, c.id});
    REQUIRE(
        backdoor::commit(store, ref_batch({a, root}, "main", std::nullopt, root.id)).has_value());
    const tree_links links;
    const result<verify_report> report =
        store.verify(verify_scope{.hashes = true, .reachability = true, .links = &links});
    REQUIRE(report.has_value());
    REQUIRE(report->findings.size() == 1);
    CHECK(report->findings[0].code == errc::object_missing);
    CHECK(report->findings[0].id == c.id);
    CHECK(report->findings[0].ref == "main");
    const result<verify_report> hashes_only =
        store.verify(verify_scope{.hashes = true, .reachability = false, .links = &links});
    REQUIRE(hashes_only.has_value());
    CHECK(hashes_only->ok());
  }

  SUBCASE("readers see whole commits while a writer commits") {
    REQUIRE(backdoor::commit(store, ref_batch({a}, "main", std::nullopt, a.id)).has_value());
    constexpr int commits = 40;
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
      readers.emplace_back([&store, &done, &torn] {
        while (!done.load()) {
          const result<digest> head = store.read_ref("main");
          if (!head || !store.get(*head).has_value()) {
            torn.fetch_add(1);
          }
        }
      });
    }
    digest previous = a.id;
    for (int i = 0; i < commits; ++i) {
      const object next = blob("step " + std::to_string(i));
      const result<commit_ack> ack =
          backdoor::commit(store, ref_batch({next}, "main", previous, next.id));
      CHECK(ack.has_value());
      previous = next.id;
    }
    done.store(true);
    for (std::thread &r : readers) {
      r.join();
    }
    CHECK(torn.load() == 0);
    CHECK(store.read_ref("main").value_or(digest{}) == previous);
  }
}

} // namespace sl::fabric::testing
