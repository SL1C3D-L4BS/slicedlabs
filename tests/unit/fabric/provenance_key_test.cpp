// FAB-INV-PRV-04 (SL-ARCH-001 ARCH1-R44): the derivation key is SHA-256 over the SLB/1 encoding of
// {sorted inputs, toolchain id, parameters digest, environment hash}: pure, independent of input
// order, sensitive to every field.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/provenance.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
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

digest dg(std::string_view text) {
  return digest{.algo = hash_algo::sha256, .bytes = sha256::hash(text)};
}

record_id in(std::string_view key) { return tg::test_id(id_class::asset, key); }

struct parts {
  std::vector<record_id> ids{in("key/a"), in("key/b"), in("key/c")};
  std::vector<digest> revs{dg("ra"), dg("rb"), dg("rc")};
  std::string tool = "cook-mesh@3.1.0";
  digest params = dg("parameters");
  digest env = dg("environment");

  [[nodiscard]] provenance build() const {
    return provenance{.input_ids = ids,
                      .input_revisions = revs,
                      .toolchain_id = tool,
                      .parameters_digest = params,
                      .environment_hash = env};
  }
};

provenance base() { return parts{}.build(); }

digest key_of(const provenance &p) { return must(derivation_key(p)); }

} // namespace

TEST_CASE("FAB-INV-PRV-04: the key is the SHA-256 of the SLB/1 preimage and is pure") {
  const provenance p = base();
  const auto preimage = must(derivation_preimage(p));
  CHECK(key_of(p) == digest{.algo = hash_algo::sha256, .bytes = sha256::hash(preimage)});
  for (int i = 0; i < 3; ++i) {
    CHECK(key_of(p) == key_of(base()));
    CHECK(must(derivation_preimage(base())) == preimage);
  }
}

TEST_CASE("FAB-INV-PRV-04: the key equals the Python hashlib golden value") {
  // Rebuilt independently with tools/fabric/slb_ref.py (python3 -I): ids are the raw bytes of
  // test_id(asset, "key/a|b|c") below, digests are hashlib.sha256 of "ra", "rb", "rc",
  // "parameters", "environment", and
  //   v = Struct((1, List(Struct((1, Id(id)), (2, Digest(1, rev))) sorted by (id, rev))),
  //              (2, Str("cook-mesh@3.1.0")), (3, Digest(1, params)), (4, Digest(1, env)))
  //   hashlib.sha256(slb_ref.encode(v)).hexdigest()
  const provenance p = base();
  const std::vector<std::string_view> id_hex = {"040278afc37aaad76c9c7b66fec96eda",
                                                "0402f1650efab04aeb10bab41a4cdc29",
                                                "0402a992fb28f5e4f9798bb68cf8632c"};
  const parts golden;
  const std::vector<record_id> &ids = golden.ids;
  REQUIRE(ids.size() == id_hex.size());
  for (std::size_t i = 0; i < id_hex.size(); ++i) {
    std::string hex;
    for (const std::uint8_t b : ids[i].bytes()) {
      hex += std::format("{:02x}", static_cast<unsigned>(b));
    }
    CHECK(hex == id_hex[i]);
  }
  CHECK(key_of(p).to_hex() == "2ff926efb784600c7c1c9aa8df8299ec4593ee1368a26e9a9c0d9cd375f83fa7");
}

TEST_CASE("FAB-INV-PRV-04: the key does not depend on the order of the inputs") {
  const parts p;
  const digest k = key_of(p.build());
  std::vector<std::size_t> order = {0, 1, 2};
  std::set<std::string> seen;
  do {
    parts q = p;
    for (std::size_t i = 0; i < 3; ++i) {
      q.ids[i] = p.ids[order[i]];
      q.revs[i] = p.revs[order[i]];
    }
    CHECK(key_of(q.build()) == k);
    seen.insert(key_of(q.build()).to_hex());
  } while (std::ranges::next_permutation(order).found);
  CHECK(seen.size() == 1);
}

TEST_CASE("FAB-INV-PRV-04: the key changes with every field") {
  const digest k = key_of(base());
  std::set<std::string> keys{k.to_hex()};
  auto changed = [&](auto &&edit) {
    parts q;
    edit(q);
    const digest kq = key_of(q.build());
    CHECK(kq != k);
    keys.insert(kq.to_hex());
  };
  changed([](parts &q) { q.ids[1] = in("key/other"); });
  changed([](parts &q) { q.revs[2] = dg("rc2"); });
  changed([](parts &q) { q.tool = "cook-mesh@3.1.1"; });
  changed([](parts &q) { q.params = dg("parameters2"); });
  changed([](parts &q) { q.env = dg("environment2"); });
  changed([](parts &q) { // adding an input
    q.ids.push_back(in("key/d"));
    q.revs.push_back(dg("rd"));
  });
  changed([](parts &q) { // dropping an input
    q.ids.pop_back();
    q.revs.pop_back();
  });
  changed([](parts &q) { // swapping the revisions between two ids re-pairs them
    std::swap(q.revs[0], q.revs[1]);
  });
  changed([](parts &q) { // parameters and environment are not interchangeable
    std::swap(q.params, q.env);
  });
  CHECK(keys.size() == 10);
}

TEST_CASE("FAB-INV-PRV-04: an incomplete record has no key") {
  provenance p = base();
  p.environment_hash.reset();
  const auto r = derivation_key(p);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::provenance_incomplete);
  CHECK(r.error().message.contains("environment_hash"));
  CHECK(derivation_preimage(p).error().code == sl::errc::provenance_incomplete);
}
