// FAB-INV-PAT-02 (SL-ARCH-002 ARCH2-R45): the footprint covers every record and path an op
// reads or writes. The validation context, lock authority and receipt builder of validate.hpp
// consume footprints and are checked here too.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
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
#include <vector>

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

bool prefix_of(const path &p, const path &of) {
  return p.size() <= of.size() && std::equal(p.begin(), p.end(), of.begin());
}

path child(const path &at, path_segment s) {
  path p = at;
  p.push_back(std::move(s));
  return p;
}

/// The minimal paths at which `a` and `b` differ (a container of one shape is entered).
void diff(const path &at, const value &a, const value &b, std::vector<path> &out) {
  if (a == b) {
    return;
  }
  if (a.kind() != b.kind() || (a.kind() == value_kind::variant && a.case_no() != b.case_no())) {
    out.push_back(at);
    return;
  }
  switch (a.kind()) {
  case value_kind::struct_: {
    std::set<std::uint32_t> nos;
    for (const value *v : {&a, &b}) {
      for (const field &f : v->fields()) {
        nos.insert(f.no);
      }
    }
    for (const std::uint32_t no : nos) {
      const value *x = a.find_field(no);
      const value *y = b.find_field(no);
      if (x != nullptr && y != nullptr) {
        diff(child(at, path_segment::field(no)), *x, *y, out);
      } else {
        out.push_back(child(at, path_segment::field(no)));
      }
    }
    break;
  }
  case value_kind::map: {
    std::vector<value> keys;
    for (const value *v : {&a, &b}) {
      for (const map_entry &e : v->entries()) {
        keys.push_back(e.key);
      }
    }
    for (const value &k : keys) {
      const value *x = a.find_key(k);
      const value *y = b.find_key(k);
      const path p = child(at, must(path_segment::key(k)));
      if (x != nullptr && y != nullptr) {
        diff(p, *x, *y, out);
      } else {
        out.push_back(p);
      }
    }
    break;
  }
  case value_kind::oset: {
    std::vector<id_bytes> elems;
    for (const value *v : {&a, &b}) {
      for (const oset_entry &e : v->elements()) {
        elems.push_back(e.element);
      }
    }
    for (const id_bytes &el : elems) {
      const oset_entry *x = a.find_element(el);
      const oset_entry *y = b.find_element(el);
      const path p = child(at, path_segment::element(el));
      if (x != nullptr && y != nullptr && x->order_key == y->order_key) {
        diff(p, x->v, y->v, out);
      } else if (x == nullptr || y == nullptr || *x != *y) {
        out.push_back(p);
      }
    }
    break;
  }
  case value_kind::variant:
    diff(child(at, path_segment::field(a.case_no())), a.payload(), b.payload(), out);
    break;
  default:
    out.push_back(at);
    break;
  }
}

bool covered(const footprint &fp, const record_id &record, const path &p, bool writing) {
  return std::ranges::any_of(fp.entries, [&](const footprint_entry &e) {
    return e.record == record && (!writing || e.mode != access::read) && prefix_of(e.path, p);
  });
}

/// The path an op reads its expectation at.
path read_path(const patch_op &op) {
  switch (op.kind) {
  case op_kind::insert_elem:
  case op_kind::remove_elem:
  case op_kind::move_elem:
    return child(op.path, path_segment::element(op.elem));
  case op_kind::set:
  case op_kind::unset:
    return op.path;
  default:
    return {};
  }
}

const record_id hero = tg::test_id(id_class::entity, "fp/hero");

footprint_entry entry(const path &p, access mode, const record_id &r = hero) {
  return footprint_entry{.record = r, .path = p, .mode = mode};
}

class fixed_validator final : public validator {
public:
  fixed_validator(std::string name, validator_stage stage, std::vector<diagnostic> out,
                  bool fail = false)
      : name_(std::move(name)), stage_(stage), out_(std::move(out)), fail_(fail) {}
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] validator_stage stage() const noexcept override { return stage_; }
  [[nodiscard]] sl::result<void> run(const validation_context &ctx,
                                     std::vector<diagnostic> &out) const override {
    if (fail_) {
      return sl::make_error(sl::errc::store_io, "could not read");
    }
    CHECK(ctx.footprint() == footprint_of(ctx.patch()));
    out.insert(out.end(), out_.begin(), out_.end());
    return {};
  }

private:
  std::string name_;
  validator_stage stage_;
  std::vector<diagnostic> out_;
  bool fail_;
};

/// Denies any footprint entry inside `locked` of `record`.
class path_lock final : public lock_authority {
public:
  path_lock(record_id record, path locked) : record_(record), locked_(std::move(locked)) {}
  [[nodiscard]] sl::result<void> check(const edit_origin & /*origin*/,
                                       const footprint &fp) const override {
    for (const footprint_entry &e : fp.entries) {
      if (conflicts(e,
                    footprint_entry{.record = record_, .path = locked_, .mode = access::write})) {
        return sl::make_error(sl::errc::locked, "held by another actor");
      }
    }
    return {};
  }

private:
  record_id record_;
  path locked_;
};

class summary_observer final : public commit_observer {
public:
  [[nodiscard]] sl::result<void> on_commit(const commit_event &event,
                                           receipt_builder &receipt) override {
    return receipt.attach("test.changed", value::uint(event.changed.size()));
  }
};

} // namespace

TEST_CASE("FAB-INV-PAT-02: 500 seeded patches: every change and read lies inside the footprint") {
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 5000);
    const tg::oracle_store base = must(gen.state(5));
    const patch n = must(normalize(must(gen.next(base)), base));
    const footprint whole = footprint_of(n);
    CHECK(std::ranges::is_sorted(whole.entries));
    CHECK(std::ranges::adjacent_find(whole.entries) == whole.entries.end());

    staged_state staged(base);
    for (std::size_t i = 0; i < n.ops.size(); ++i) {
      CAPTURE(i);
      const patch_op &op = n.ops[i];
      const footprint fp = footprint_of(op);
      for (const footprint_entry &e : fp.entries) {
        CHECK(std::ranges::binary_search(whole.entries, e));
      }
      if (op.kind == op_kind::annotate) {
        CHECK(fp.entries.empty());
        must_ok(staged.apply(op));
        continue;
      }
      CHECK(covered(fp, op.target, read_path(op), false));
      const std::optional<record_state> before = must(staged.read(op.target));
      must_ok(staged.apply(op));
      const std::optional<record_state> after = must(staged.read(op.target));
      if (before.has_value() != after.has_value() || (before && before->type != after->type)) {
        CHECK(covered(fp, op.target, {}, true));
        continue;
      }
      std::vector<path> changed;
      if (before && after) {
        diff({}, before->body, after->body, changed);
      }
      for (const path &p : changed) {
        CAPTURE(to_string(p));
        CHECK(covered(fp, op.target, p, true));
      }
    }
    // Records outside the footprint are untouched.
    const tg::oracle_store after = tg::materialize(base, staged);
    std::set<record_id> ids;
    for (const tg::oracle_store *s : {&base, &after}) {
      for (const auto &kv : s->records()) {
        ids.insert(kv.first);
      }
    }
    for (const record_id &id : ids) {
      const bool named = std::ranges::any_of(
          whole.entries, [&](const footprint_entry &e) { return e.record == id; });
      if (!named) {
        CHECK(must(base.read(id)) == must(after.read(id)));
      }
    }
  }
}

TEST_CASE("FAB-INV-PAT-02: the footprint table of SL-ARCH-002 ops") {
  const id_bytes el = tg::test_id(id_class::element, "fp/el").bytes();
  const path comps{path_segment::field(4)};
  const path at_el = child(comps, path_segment::element(el));
  const path name{path_segment::field(1)};
  const value v = value::uint(1);
  struct row {
    patch_op op;
    footprint_entry expected;
  };
  const std::vector<row> rows = {
      {.op = op_create(hero, type_id_of(tt::entity), v), .expected = entry({}, access::create)},
      {.op = op_delete(hero), .expected = entry({}, access::remove)},
      {.op = op_set(hero, name, v), .expected = entry(name, access::write)},
      {.op = op_unset(hero, name), .expected = entry(name, access::write)},
      {.op = op_insert(hero, comps, el, {0x40}, v), .expected = entry(at_el, access::create)},
      {.op = op_remove(hero, comps, el), .expected = entry(at_el, access::remove)},
      {.op = op_move(hero, comps, el, {0x40}), .expected = entry(at_el, access::write)},
      {.op = op_retype(hero, type_id_of(tt::scene), v), .expected = entry({}, access::write)},
  };
  for (const row &r : rows) {
    CAPTURE(to_string(r.op.kind));
    const footprint fp = footprint_of(r.op);
    REQUIRE(fp.entries.size() == 1);
    CHECK(fp.entries[0] == r.expected);
  }
  CHECK(footprint_of(op_annotate(v)).entries.empty());
}

TEST_CASE("FAB-INV-PAT-02: entries conflict on one record when a path is a prefix of the other") {
  const path a{path_segment::field(1)};
  const path ab{path_segment::field(1), path_segment::field(2)};
  const path c{path_segment::field(3)};
  const record_id other = tg::test_id(id_class::entity, "fp/other");
  CHECK(conflicts(entry(a, access::write), entry(ab, access::write)));
  CHECK(conflicts(entry(ab, access::read), entry(a, access::write)));
  CHECK(conflicts(entry({}, access::remove), entry(ab, access::read)));
  CHECK(conflicts(entry(a, access::create), entry(a, access::create)));
  CHECK_FALSE(conflicts(entry(a, access::read), entry(ab, access::read)));
  CHECK_FALSE(conflicts(entry(a, access::write), entry(c, access::write)));
  CHECK_FALSE(conflicts(entry(a, access::write), entry(a, access::write, other)));

  footprint f;
  f.add(entry(c, access::write));
  f.add(entry(a, access::write));
  f.add(entry(a, access::write));
  CHECK(f.entries.size() == 2);
  CHECK(std::ranges::is_sorted(f.entries));
  footprint g;
  g.add(entry(ab, access::read));
  g.add(entry(c, access::write, other));
  const auto overlaps = f.overlaps(g);
  REQUIRE(overlaps.size() == 1);
  CHECK(overlaps[0].first == entry(a, access::write));
  CHECK(overlaps[0].second == entry(ab, access::read));
}

TEST_CASE("FAB-INV-PAT-02: validators see the footprint; diagnostics come back in stage order") {
  tg::oracle_store base;
  base.put(hero,
           record_state{.type = type_id_of(tt::entity), .body = must(tg::sample_entity("h"))});
  patch p;
  p.ops.push_back(op_set(hero, path{path_segment::field(2)}, value::uint(1)));
  const patch n = must(normalize(p, base));
  staged_state after(base);
  must_ok(after.apply(n));
  const schema_registry reg = must(tg::make_test_registry());
  const validation_context ctx(n, after, reg, edit_origin{.actor = {}, .subsystem = "test"});
  CHECK(ctx.footprint() == footprint_of(n));
  CHECK(must(ctx.before().read(hero)) == must(base.read(hero)));
  CHECK(must_have(must(ctx.after().read(hero))).body.has_field(2));

  const record_id r1 = tg::test_id(id_class::entity, "fp/r1");
  const diagnostic late{.rule = "R-B",
                        .severity = severity::warning,
                        .record = r1,
                        .path = {},
                        .message = "late",
                        .stage = validator_stage::structural};
  const diagnostic early{.rule = "R-A",
                         .severity = severity::error,
                         .record = r1,
                         .path = {},
                         .message = "early",
                         .stage = validator_stage::structural};
  validator_registry vr;
  must_ok(vr.add(std::make_shared<fixed_validator>("v.schema", validator_stage::schema,
                                                   std::vector<diagnostic>{late, early})));
  must_ok(vr.add(std::make_shared<fixed_validator>("v.structural", validator_stage::structural,
                                                   std::vector<diagnostic>{late})));
  CHECK(vr.add(std::make_shared<fixed_validator>("v.schema", validator_stage::locks,
                                                 std::vector<diagnostic>{}))
            .error()
            .code == sl::errc::already_exists);
  CHECK(vr.add(nullptr).error().code == sl::errc::invalid_argument);
  const auto order = vr.in_order();
  REQUIRE(order.size() == 2);
  CHECK(order[0]->name() == "v.structural");

  const std::vector<diagnostic> found = must(vr.run(ctx));
  REQUIRE(found.size() == 3);
  CHECK(found[0].stage == validator_stage::structural);
  CHECK(found[1].stage == validator_stage::schema);
  CHECK(found[1].rule == "R-A"); // within a stage: record, path, then rule
  CHECK(found[2].rule == "R-B");
  CHECK(std::ranges::is_sorted(found, diagnostic_less));
  CHECK(has_error(found));

  vr.freeze();
  CHECK(vr.add(std::make_shared<fixed_validator>("v.late", validator_stage::policy,
                                                 std::vector<diagnostic>{}))
            .error()
            .code == sl::errc::registry_frozen);

  validator_registry failing;
  must_ok(failing.add(std::make_shared<fixed_validator>("v.io", validator_stage::references,
                                                        std::vector<diagnostic>{}, true)));
  const auto r = failing.run(ctx);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message.find("v.io") != std::string::npos);
}

TEST_CASE("FAB-INV-PAT-02: lock authorities judge footprints; observers fill the receipt") {
  const edit_origin origin{.actor = {}, .subsystem = "test"};
  patch p;
  p.ops.push_back(
      op_set(hero, path{path_segment::field(1), path_segment::field(2)}, value::uint(1)));
  const footprint fp = footprint_of(p);
  CHECK(null_lock_authority{}.check(origin, fp).has_value());
  const path_lock lock_name(hero, path{path_segment::field(1)});
  const auto denied = lock_name.check(origin, fp);
  REQUIRE_FALSE(denied.has_value());
  CHECK(denied.error().code == sl::errc::locked);
  CHECK(path_lock(hero, path{path_segment::field(3)}).check(origin, fp).has_value());

  commit_event event;
  event.patch = p;
  event.changed = {hero};
  receipt_builder receipt;
  summary_observer obs;
  must_ok(obs.on_commit(event, receipt));
  REQUIRE(receipt.find("test.changed") != nullptr);
  CHECK(*receipt.find("test.changed") == value::uint(1));
  CHECK(obs.on_commit(event, receipt).error().code == sl::errc::already_exists);
  CHECK(receipt.attachments().size() == 1);
  CHECK(receipt.find("absent") == nullptr);
}
