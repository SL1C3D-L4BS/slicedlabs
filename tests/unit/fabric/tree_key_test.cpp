// FAB-INV-REV-02 (SL-ARCH-001 ARCH1-R51): trie keys are SHA-256("slf-key/1" || id)[0:16], and ids
// that share class and scheme bytes spread across the root fan-out. Keys are checked against an
// in-test concatenation and against Python hashlib vectors; 4096 derived entity ids (one class,
// one scheme) fill every root nibble within a chi-square bound and make a trie whose root branch
// has all 16 children, where the raw ids would all fall under one nibble.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/tree.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

std::string hex(const trie_key &k) { return to_hex(byte_span(k)); }

} // namespace

TEST_CASE("FAB-INV-REV-02: trie key is the first 16 bytes of SHA-256('slf-key/1' || id)") {
  for (const record_id &id : testing::tree_ids(64, id_class::asset, "key")) {
    std::vector<std::uint8_t> pre(trie_key_domain.begin(), trie_key_domain.end());
    pre.insert(pre.end(), id.bytes().begin(), id.bytes().end());
    const auto full = sha256::hash(byte_span(pre));
    trie_key expect{};
    std::copy_n(full.begin(), expect.size(), expect.begin());
    CHECK(key_of(id) == expect);
  }
  // Python: hashlib.sha256(b"slf-key/1" + id_bytes).hexdigest()[:32]
  CHECK(hex(key_of(well_known(id_class::entity, 1))) == "8da62e7faeb87de605eeb3d9a4b67d23");
  CHECK(hex(key_of(well_known(id_class::namespace_, 1))) == "189cd70119da97c7bd196ce8b107bc6f");
  CHECK(hex(key_of(well_known(id_class::asset, 7))) == "4d0c33cf8b13656af03bf5d6d498a5e4");
}

TEST_CASE("FAB-INV-REV-02: key nibbles read high nibble first and with_nibble sets one nibble") {
  trie_key k{};
  k[0] = 0xAB;
  k[15] = 0xCD;
  CHECK(key_nibble(k, 0) == 0xAU);
  CHECK(key_nibble(k, 1) == 0xBU);
  CHECK(key_nibble(k, 30) == 0xCU);
  CHECK(key_nibble(k, 31) == 0xDU);
  const trie_key j = detail::with_nibble(k, 1, 0x3U);
  CHECK(j[0] == 0xA3);
  CHECK(detail::with_nibble(j, 0, 0x0U)[0] == 0x03);
  CHECK(detail::with_nibble(k, 31, 0x1U)[15] == 0xC1);
}

TEST_CASE("FAB-INV-REV-02: ids of one class and scheme spread across the root fan-out") {
  constexpr std::size_t n = 4096;
  const std::vector<record_id> ids = testing::tree_ids(n, id_class::entity, "spread");
  std::vector<std::size_t> hashed(16);
  std::vector<std::size_t> raw(16);
  for (const record_id &id : ids) {
    ++hashed[key_nibble(key_of(id), 0)];
    ++raw[id.bytes()[0] >> 4U];
  }
  // Raw ids: the class byte is shared, so every id sits under one root nibble (ADR-0014).
  CHECK(raw[0] == n);
  // Hashed keys: chi-square over 16 cells (15 degrees of freedom; 0.999 quantile is 37.7).
  const double expected = static_cast<double>(n) / 16.0;
  double chi = 0.0;
  for (const std::size_t c : hashed) {
    CHECK(c > 0);
    const double d = static_cast<double>(c) - expected;
    chi += d * d / expected;
  }
  CHECK(chi < 37.7);

  memory_backend store;
  object_sink sink;
  std::vector<tree_entry> entries;
  entries.reserve(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    entries.push_back(testing::record_entry(ids[i], i));
  }
  const subtree t = must(build_tree(entries, sink));
  CHECK(t.count == n);
  REQUIRE(testing::commit_objects(store, sink.take()).has_value());
  const tree_reader reader(store);
  const tree_node root = must(reader.load(t.id));
  CHECK(root.bitmap == 0xFFFFU);
  CHECK(root.children.size() == 16);
  for (std::size_t i = 0; i < ids.size(); i += 16) {
    const record_id &id = ids[i];
    const auto e = must(reader.find(t.id, id));
    REQUIRE(e.has_value());
    CHECK(e.value_or(tree_entry{}).id == id);
  }
}
