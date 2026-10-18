// FAB-INV-MRG-04 (SL-ARCH-002 ARCH2-R47): the default merge driver merges disjoint field edits
// and reports overlapping ones.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
namespace tt = sl::fabric::testing::test_types;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

/// The held value of `o` (a failed REQUIRE otherwise).
template <class T> T must_have(const std::optional<T> &o) {
  REQUIRE(o.has_value());
  return o.value_or(T{});
}

const record_id hero = tg::test_id(id_class::entity, "merge/hero");
path name() { return path{path_segment::field(1)}; }
path layer() { return path{path_segment::field(2)}; }
path comps() { return path{path_segment::field(4)}; }
path props() { return path{path_segment::field(5)}; }

value base_body() { return must(tg::sample_entity("hero")); }

value edit(const value &body, const path &p, value v) { return must(body.with(p, std::move(v))); }

merge_result merge(const value &base, const std::optional<value> &ours,
                   const std::optional<value> &theirs) {
  const field_merge_driver driver;
  return must(driver.merge(merge_input{.record = hero,
                                       .type = type_id_of(tt::entity),
                                       .base = base,
                                       .ours = ours,
                                       .theirs = theirs}));
}

path key_path(const char *k) {
  return path{path_segment::field(5), must(path_segment::key(must(value::string(k))))};
}

class keep_ours final : public merge_driver {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test.keep_ours"; }
  [[nodiscard]] sl::result<merge_result> merge(const merge_input &in) const override {
    return merge_result{.merged = in.ours, .conflicts = {}};
  }
};

} // namespace

TEST_CASE("FAB-INV-MRG-04: disjoint field edits merge; overlapping ones are reported") {
  const value b = base_body();
  const value ours = edit(b, name(), must(value::string("ours")));
  const value theirs = edit(b, layer(), value::uint(2));

  const merge_result clean = merge(b, ours, theirs);
  CHECK(clean.clean());
  REQUIRE(clean.merged);
  CHECK(must(must_have(clean.merged).get(name())) == must(value::string("ours")));
  CHECK(must(must_have(clean.merged).get(layer())) == value::uint(2));

  const value theirs_name = edit(b, name(), must(value::string("theirs")));
  const merge_result clash = merge(b, ours, theirs_name);
  REQUIRE(clash.conflicts.size() == 1);
  CHECK(clash.conflicts[0].path == name());
  CHECK(clash.conflicts[0].base == must(value::string("hero")));
  CHECK(clash.conflicts[0].ours == must(value::string("ours")));
  CHECK(clash.conflicts[0].theirs == must(value::string("theirs")));
  CHECK(must(must_have(clash.merged).get(name())) == must(value::string("ours"))); // ours kept

  // The same edit on both sides agrees.
  CHECK(merge(b, ours, ours).clean());
  // One side unchanged yields the other.
  CHECK(must_have(merge(b, b, theirs).merged) == theirs);
}

TEST_CASE("FAB-INV-MRG-04: maps by key, osets by element, variants by case") {
  const value b = base_body();
  const id_bytes tag = tg::test_id(id_class::element, "hero/tag").bytes();
  const id_bytes xform = tg::test_id(id_class::element, "hero/transform").bytes();
  const id_bytes added_o = tg::test_id(id_class::element, "merge/o").bytes();
  const id_bytes added_t = tg::test_id(id_class::element, "merge/t").bytes();

  // Map: different keys merge, the same key edited twice conflicts.
  const value mo = edit(b, key_path("a"), must(value::string("1")));
  const value mt = edit(b, key_path("b"), must(value::string("2")));
  const merge_result maps = merge(b, mo, mt);
  CHECK(maps.clean());
  CHECK(must(must_have(maps.merged).get(props())).entries().size() == 3);
  const value mo2 = edit(b, key_path("owner"), must(value::string("x")));
  const value mt2 = edit(b, key_path("owner"), must(value::string("y")));
  REQUIRE(merge(b, mo2, mt2).conflicts.size() == 1);
  CHECK(merge(b, mo2, mt2).conflicts[0].path == key_path("owner"));

  // Oset: concurrent inserts, a move against a value edit of another element, both kept.
  const value oo = must(must(b.insert_element(comps(), added_o, {0x60}, value::uint(1)))
                            .move_element(comps(), tag, {0x20}));
  const value ot = must(must(b.insert_element(comps(), added_t, {0x60}, value::uint(2)))
                            .with(path{path_segment::field(4), path_segment::element(xform)},
                                  must(value::variant(3, must(value::struct_({}))))));
  const merge_result osets = merge(b, oo, ot);
  CHECK(osets.clean());
  const value merged_oset = must(must_have(osets.merged).get(comps()));
  CHECK(merged_oset.elements().size() == 4);
  CHECK(merged_oset.find_element(tag)->order_key == std::vector<std::uint8_t>{0x20});
  CHECK(merged_oset.find_element(xform)->v.case_no() == 3);

  // Both sides move one element to different keys: an order conflict at the element.
  const value mv_o = must(b.move_element(comps(), tag, {0x10}));
  const value mv_t = must(b.move_element(comps(), tag, {0x90}));
  const merge_result moves = merge(b, mv_o, mv_t);
  REQUIRE(moves.conflicts.size() == 1);
  CHECK(moves.conflicts[0].path == path{path_segment::field(4), path_segment::element(tag)});

  // Variant: edits inside one case merge; a case change against an edit conflicts.
  const path xform_pos{path_segment::field(4), path_segment::element(xform), path_segment::field(1),
                       path_segment::field(2)};
  const path xform_scale{path_segment::field(4), path_segment::element(xform),
                         path_segment::field(1), path_segment::field(1)};
  const value vo = edit(b, xform_pos, must(value::f64(5.0)));
  const value vt = edit(b, xform_scale, must(tg::make_vec3(0, 0, 0)));
  CHECK(merge(b, vo, vt).clean());
  const value case_change = edit(b, path{path_segment::field(4), path_segment::element(xform)},
                                 must(value::variant(3, must(value::struct_({})))));
  CHECK_FALSE(merge(b, vo, case_change).clean());
}

TEST_CASE("FAB-INV-MRG-04: delete against edit conflicts at the record root") {
  const value b = base_body();
  const merge_result r = merge(b, std::nullopt, edit(b, name(), must(value::string("x"))));
  REQUIRE(r.conflicts.size() == 1);
  CHECK(r.conflicts[0].path.empty());
  CHECK_FALSE(r.merged.has_value()); // ours (deleted) kept
  // Both delete: agreed.
  CHECK(merge(b, std::nullopt, std::nullopt).clean());
  // Delete against no change: deleted.
  CHECK_FALSE(merge(b, std::nullopt, b).merged.has_value());
}

TEST_CASE("FAB-INV-MRG-04: 300 seeded disjoint patch pairs merge to their sequential result") {
  std::size_t merged_pairs = 0;
  for (std::uint64_t seed = 0; seed < 300; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 15000);
    const tg::oracle_store base = must(gen.state(1));
    const patch p = must(normalize(must(gen.next(base)), base));
    const patch q = must(normalize(must(gen.next(base)), base));
    const footprint fp = footprint_of(p);
    const footprint fq = footprint_of(q);
    if (!commutes(fp, fq)) {
      continue;
    }
    tg::oracle_store ours = base;
    must_ok(ours.apply(p));
    tg::oracle_store theirs = base;
    must_ok(theirs.apply(q));
    tg::oracle_store both = ours;
    must_ok(both.apply(q));
    std::set<record_id> touched;
    for (const footprint *f : {&fp, &fq}) {
      for (const footprint_entry &e : f->entries) {
        touched.insert(e.record);
      }
    }
    merge_driver_registry drivers;
    for (const record_id &id : touched) {
      const auto b = must(base.read(id));
      const auto o = must(ours.read(id));
      const auto t = must(theirs.read(id));
      const auto want = must(both.read(id));
      type_id type;
      if (o) {
        type = o->type;
      } else if (t) {
        type = t->type;
      }
      const merge_result r = must(drivers.driver_for(type).merge(
          merge_input{.record = id,
                      .type = type,
                      .base = b ? std::optional<value>(b->body) : std::nullopt,
                      .ours = o ? std::optional<value>(o->body) : std::nullopt,
                      .theirs = t ? std::optional<value>(t->body) : std::nullopt}));
      CHECK(r.clean());
      CHECK(r.merged == (want ? std::optional<value>(want->body) : std::nullopt));
    }
    ++merged_pairs;
  }
  CHECK(merged_pairs > 30);
}

TEST_CASE("FAB-INV-MRG-04: the registry is keyed by type id with the field driver as default") {
  merge_driver_registry reg;
  const type_id entity = type_id_of(tt::entity);
  const type_id scene = type_id_of(tt::scene);
  CHECK(reg.driver_for(entity).name() == "fabric.field");
  CHECK(&reg.driver_for(entity) == &reg.default_driver());
  must_ok(reg.add(scene, std::make_shared<keep_ours>()));
  CHECK(reg.driver_for(scene).name() == "test.keep_ours");
  CHECK(reg.driver_for(entity).name() == "fabric.field");
  CHECK(reg.add(scene, std::make_shared<keep_ours>()).error().code == sl::errc::already_exists);
  CHECK(reg.add(entity, nullptr).error().code == sl::errc::invalid_argument);
  reg.freeze();
  CHECK(reg.frozen());
  CHECK(reg.add(entity, std::make_shared<keep_ours>()).error().code == sl::errc::registry_frozen);
}
