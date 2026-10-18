// FAB-INV-ID-03 (SL-ARCH-001 ARCH1-R03): derive, well_known and imported are pure functions of
// their inputs, equal the Python hashlib values, and are domain separated by class and scheme.

#include "sl/fabric/id.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

using sl::fabric::id_class;
using sl::fabric::mint_scheme;
using sl::fabric::record_id;

} // namespace

TEST_CASE("FAB-INV-ID-03: derived, well-known and imported ids equal the hashlib values") {
  const record_id root_ns = sl::fabric::well_known(id_class::namespace_, 1);
  CHECK(sl::fabric::derive(id_class::type, root_ns, "sl.core.Transform").to_string() ==
        "typ_030A2Z0TSNAWHYKW7BMB3PNW9Y");
  const record_id manifest = sl::fabric::well_known(id_class::entity, 1);
  CHECK(sl::fabric::derive(id_class::instance, manifest, "").to_string() ==
        "ins_060A0VQQGH9NSQ9GT4T5J7YAGY");
  CHECK(sl::fabric::imported(id_class::asset, "fbx", "mesh/Body").to_string() ==
        "ast_040H8K7EAPD0497F38ENE8DHJK");
}

TEST_CASE("FAB-INV-ID-03: the functions are pure") {
  const record_id parent = sl::fabric::well_known(id_class::entity, 9);
  for (int i = 0; i < 3; ++i) {
    CHECK(sl::fabric::derive(id_class::instance, parent, "child") ==
          sl::fabric::derive(id_class::instance, parent, "child"));
    CHECK(sl::fabric::imported(id_class::asset, "gltf", "a") ==
          sl::fabric::imported(id_class::asset, "gltf", "a"));
    CHECK(sl::fabric::well_known(id_class::actor, 3) == sl::fabric::well_known(id_class::actor, 3));
  }
}

TEST_CASE("FAB-INV-ID-03: class, scheme and layout of each scheme") {
  const record_id wk = sl::fabric::well_known(id_class::namespace_, 0x01020304U);
  CHECK(wk.cls() == id_class::namespace_);
  CHECK(wk.scheme() == mint_scheme::well_known);
  const sl::fabric::byte_span middle = sl::fabric::byte_span{wk.bytes()}.subspan(2, 10);
  CHECK(std::ranges::all_of(middle, [](std::uint8_t b) { return b == 0; }));
  CHECK(wk.bytes()[12] == 0x01);
  CHECK(wk.bytes()[15] == 0x04);
  const record_id d = sl::fabric::derive(id_class::derived, wk, "k");
  CHECK(d.cls() == id_class::derived);
  CHECK(d.scheme() == mint_scheme::derived);
  const record_id im = sl::fabric::imported(id_class::import, "s", "e");
  CHECK(im.cls() == id_class::import);
  CHECK(im.scheme() == mint_scheme::imported);
}

TEST_CASE("FAB-INV-ID-03: ids are domain separated by class, scheme and every input") {
  const record_id parent = sl::fabric::well_known(id_class::entity, 1);
  std::set<record_id> seen;
  std::set<std::string> payloads; // bytes 2-15: equal payloads would mean no class separation
  for (std::uint8_t n = 1; n <= 15; ++n) {
    const auto c = static_cast<id_class>(n);
    for (const record_id &id :
         {sl::fabric::derive(c, parent, "key"), sl::fabric::imported(c, "sys", "key")}) {
      CHECK(seen.insert(id).second);
      CHECK(payloads.insert(std::string(id.bytes().begin() + 2, id.bytes().end())).second);
    }
    CHECK(seen.insert(sl::fabric::well_known(c, 1)).second);
  }
  // Every input moves the derived id: parent, key, and the length prefixes keep
  // ("ab", "c") and ("a", "bc") apart.
  CHECK(sl::fabric::derive(id_class::instance, parent, "a") !=
        sl::fabric::derive(id_class::instance, sl::fabric::well_known(id_class::entity, 2), "a"));
  CHECK(sl::fabric::derive(id_class::instance, parent, "a") !=
        sl::fabric::derive(id_class::instance, parent, "b"));
  CHECK(sl::fabric::imported(id_class::asset, "ab", "c") !=
        sl::fabric::imported(id_class::asset, "a", "bc"));
  CHECK(sl::fabric::imported(id_class::asset, "", "x") !=
        sl::fabric::imported(id_class::asset, "x", ""));
  CHECK(sl::fabric::well_known(id_class::asset, 1) != sl::fabric::well_known(id_class::asset, 2));
}
