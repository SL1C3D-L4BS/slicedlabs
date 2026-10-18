// FAB-INV-PAT-03 (SL-ARCH-002 ARCH2-R03): patch to value round trip keeps the version; an
// unknown op kind under the same major is rejected.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <cstdint>
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

/// `v` with the value at `p` replaced (or added) by `x`.
value edited(const value &v, const path &p, value x) { return must(v.with(p, std::move(x))); }

/// The ops list of the value form.
path ops_path() { return path{path_segment::field(4)}; }

/// The value form of a one-op patch with the op struct edited by `edit`.
template <class F> value with_op_edit(const patch &p, F edit) {
  const value v = must(to_value(p));
  const value ops = must(v.get(ops_path()));
  std::vector<value> items(ops.items().begin(), ops.items().end());
  items[0] = edit(items[0]);
  return edited(v, ops_path(), must(value::list(std::move(items))));
}

} // namespace

TEST_CASE("FAB-INV-PAT-03: 500 seeded patches round trip through their value form") {
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 4000);
    const tg::oracle_store base = must(gen.state(4));
    const patch authored = must(gen.next(base));
    const patch n = must(normalize(authored, base));
    for (const patch *p : {&authored, &n}) {
      const value v = must(to_value(*p));
      CHECK(must(to_value(*p)) == v); // deterministic
      const patch back = must(from_value(v));
      CHECK(back == *p);
      CHECK(back.version == p->version);
    }
  }
}

TEST_CASE("FAB-INV-PAT-03: the version is kept; a newer minor's extra fields are ignored") {
  patch p;
  p.version.minor = 7;
  p.ops.push_back(op_annotate(must(value::string("hello"))));
  const value v = must(to_value(p));
  CHECK(must(from_value(v)).version == patch_version{.major = 1, .minor = 7});

  // A field this reader does not know: ignored under a newer minor ...
  const value extra = edited(v, path{path_segment::field(9)}, value::uint(1));
  CHECK(must(from_value(extra)) == p);
  const value op_extra = with_op_edit(
      p, [](const value &op) { return edited(op, path{path_segment::field(40)}, value::uint(1)); });
  CHECK(must(from_value(op_extra)) == p);

  // ... and refused under a minor it knows.
  p.version.minor = patch_minor;
  const value known = must(to_value(p));
  const auto r = from_value(edited(known, path{path_segment::field(9)}, value::uint(1)));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::type_mismatch);
  const auto r_op = from_value(with_op_edit(p, [](const value &op) {
    return edited(op, path{path_segment::field(40)}, value::uint(1));
  }));
  REQUIRE_FALSE(r_op.has_value());
  CHECK(r_op.error().code == sl::errc::type_mismatch);
}

TEST_CASE("FAB-INV-PAT-03: an unknown op kind under the same major is rejected") {
  patch p;
  p.ops.push_back(op_annotate(must(value::string("x"))));
  for (const std::uint64_t kind : {0ULL, 10ULL, 200ULL}) {
    CAPTURE(kind);
    for (const std::uint32_t minor : {0U, 5U}) {
      p.version.minor = minor;
      const value v = with_op_edit(p, [&](const value &op) {
        return edited(op, path{path_segment::field(1)}, value::uint(kind));
      });
      const auto r = from_value(v);
      REQUIRE_FALSE(r.has_value());
      CHECK(r.error().code == sl::errc::format_unsupported);
      CHECK(r.error().message.find("unknown op kind") != std::string::npos);
    }
  }
  // Another major is refused whole.
  const value v2 = edited(must(to_value(p)), path{path_segment::field(1)}, value::uint(2));
  const auto r2 = from_value(v2);
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().code == sl::errc::format_unsupported);
}

TEST_CASE("FAB-INV-PAT-03: malformed values are type_mismatch") {
  CHECK(from_value(value::uint(1)).error().code == sl::errc::type_mismatch);
  CHECK(from_value(must(value::struct_({}))).error().code == sl::errc::type_mismatch);
  patch p;
  p.ops.push_back(op_set(tg::test_id(id_class::entity, "v/e"), path{path_segment::field(1)},
                         value::uint(1), expectation::absent()));
  const value bad_path = with_op_edit(
      p, [](const value &op) { return edited(op, path{path_segment::field(3)}, value::uint(3)); });
  CHECK(from_value(bad_path).error().code == sl::errc::type_mismatch);
  const value bad_expect = with_op_edit(p, [](const value &op) {
    return edited(op, path{path_segment::field(5)}, must(value::variant(9, value::uint(0))));
  });
  CHECK(from_value(bad_expect).error().code == sl::errc::type_mismatch);
  // Every path segment kind survives, keys of every key kind included.
  patch keys;
  keys.ops.push_back(
      op_set(tg::test_id(id_class::entity, "v/k"),
             path{path_segment::field(5), must(path_segment::key(must(value::string("k")))),
                  must(path_segment::key(value::nint(3))), must(path_segment::key(value::uint(4))),
                  path_segment::element(tg::test_id(id_class::element, "v/el"))},
             value::uint(1)));
  CHECK(must(from_value(must(to_value(keys)))) == keys);
}
