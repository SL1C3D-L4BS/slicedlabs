// FAB-INV-PRV-01 (SL-ARCH-001 ARCH1-R43, SL-ARCH-002 ARCH2-R14): each missing F3 field makes a
// derived record provenance_incomplete, in the checker and in the invariants-stage validator.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

digest dg(std::string_view text) {
  return digest{.algo = hash_algo::sha256, .bytes = sha256::hash(text)};
}

provenance complete() {
  return provenance{
      .input_ids = std::vector<record_id>{tg::test_id(id_class::asset, "prv/in/a"),
                                          tg::test_id(id_class::asset, "prv/in/b")},
      .input_revisions = std::vector<digest>{dg("rev-a"), dg("rev-b")},
      .toolchain_id = "cook-mesh@3.1.0",
      .parameters_digest = dg("parameters"),
      .environment_hash = dg("environment"),
  };
}

std::vector<std::string_view> names() {
  return {"input_ids", "input_revisions", "toolchain_id", "parameters_digest", "environment_hash"};
}

provenance without(std::size_t field_no) {
  provenance p = complete();
  switch (field_no) {
  case 0:
    p.input_ids.reset();
    break;
  case 1:
    p.input_revisions.reset();
    break;
  case 2:
    p.toolchain_id.reset();
    break;
  case 3:
    p.parameters_digest.reset();
    break;
  default:
    p.environment_hash.reset();
    break;
  }
  return p;
}

// ---- Validator fixture -------------------------------------------------------------------

const schema_registry &schemas() {
  static const schema_registry reg = must(tg::make_test_registry());
  return reg;
}

domain_registry make_domains() {
  domain_registry reg;
  must_ok(reg.claim(domain_spec{.name = "asset.cooked",
                                .owner = subsystem::asset,
                                .state_classes = {state_class::derived},
                                .types = {}}));
  must_ok(reg.claim(domain_spec{.name = "content.mesh",
                                .owner = subsystem::content,
                                .state_classes = {state_class::document},
                                .types = {}}));
  must_ok(reg.freeze());
  return reg;
}

const record_id cooked_id = tg::test_id(id_class::derived, "prv/cooked");
const record_id mesh_id = tg::test_id(id_class::asset, "prv/mesh");

value cooked_body() { return must(value::struct_({field{.no = 1, .v = value::id(mesh_id)}})); }

patch create_cooked() {
  patch p;
  p.ops.push_back(op_create(cooked_id, type_id_of(tg::test_types::cooked_mesh), cooked_body()));
  return p;
}

std::vector<diagnostic> judge(const patch &p, const provenance_source &source,
                              const tg::oracle_store &base = {}) {
  static const domain_registry domains = make_domains();
  actor_directory directory;
  const actor who{.id = must(actor_id::from(tg::test_id(id_class::actor, "prv/tool"))),
                  .kind = actor_kind::tool,
                  .grants = {}};
  must_ok(directory.add(who));
  const patch n = must(normalize(p, base));
  staged_state staged(base);
  must_ok(staged.apply(n));
  const validation_context ctx(n, staged, schemas(),
                               edit_origin{.actor = who.id, .subsystem = "asset"});
  const provenance_validator v(source, directory, domains);
  std::vector<diagnostic> out;
  must_ok(v.run(ctx, out));
  return out;
}

} // namespace

TEST_CASE("FAB-INV-PRV-01: a complete record passes and its field names are the five F3 fields") {
  const std::vector<std::string_view> field_names = names();
  must_ok(check_provenance(complete()));
  CHECK_FALSE(first_missing_field(complete()).has_value());
  REQUIRE(f3_field_names.size() == field_names.size());
  CHECK(std::ranges::equal(f3_field_names, field_names));
}

TEST_CASE("FAB-INV-PRV-01: each field missing in turn is provenance_incomplete naming it") {
  const std::vector<std::string_view> field_names = names();
  for (std::size_t i = 0; i < field_names.size(); ++i) {
    CAPTURE(field_names[i]);
    const provenance p = without(i);
    CHECK(first_missing_field(p) == field_names[i]);
    const auto r = check_provenance(p);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::provenance_incomplete);
    CHECK(r.error().message.contains(std::string("'") + std::string(field_names[i]) + "'"));
  }
  // Nothing at all names the first field; the checker reports one field at a time, in order.
  const auto none = check_provenance(provenance{});
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == sl::errc::provenance_incomplete);
  CHECK(none.error().message.contains("input_ids"));
}

TEST_CASE("FAB-INV-PRV-01: present but empty or zero fields count as missing") {
  provenance p = complete();
  p.input_ids = std::vector<record_id>{};
  p.input_revisions = std::vector<digest>{};
  CHECK(first_missing_field(p) == std::string_view{"input_ids"});

  p = complete();
  p.input_ids = std::vector<record_id>{record_id{}, tg::test_id(id_class::asset, "prv/in/b")};
  CHECK(first_missing_field(p) == std::string_view{"input_ids"});

  p = complete();
  p.input_revisions = std::vector<digest>{dg("rev-a")}; // one digest for two ids
  CHECK(first_missing_field(p) == std::string_view{"input_revisions"});

  p = complete();
  p.input_revisions = std::vector<digest>{dg("rev-a"), digest{}};
  CHECK(first_missing_field(p) == std::string_view{"input_revisions"});

  p = complete();
  p.toolchain_id = "";
  CHECK(first_missing_field(p) == std::string_view{"toolchain_id"});

  p = complete();
  p.parameters_digest = digest{};
  CHECK(first_missing_field(p) == std::string_view{"parameters_digest"});

  p = complete();
  p.environment_hash = digest{};
  CHECK(first_missing_field(p) == std::string_view{"environment_hash"});
}

TEST_CASE("FAB-INV-PRV-01: a record with a field missing decodes, then fails the check") {
  const std::vector<std::string_view> field_names = names();
  for (std::size_t i = 0; i < field_names.size(); ++i) {
    CAPTURE(field_names[i]);
    const value v = must(without(i).to_value());
    CHECK_FALSE(v.has_field(static_cast<std::uint32_t>(i + 1)));
    const auto decoded = provenance::from_value(v); // a missing field is not a decode error
    REQUIRE(decoded.has_value());
    CHECK(check_provenance(*decoded).error().code == sl::errc::provenance_incomplete);
  }
  const provenance whole = must(provenance::from_value(must(complete().to_value())));
  CHECK(whole == complete());
  must_ok(check_provenance(whole));
}

TEST_CASE("FAB-INV-PRV-01: the validator refuses a derived record without provenance") {
  const std::vector<std::string_view> field_names = names();
  const fixed_provenance_source nothing;
  const auto none = judge(create_cooked(), nothing);
  REQUIRE(none.size() == 1);
  CHECK(none[0].rule == "R-PRV-INCOMPLETE");
  CHECK(none[0].record == cooked_id);
  CHECK(none[0].stage == validator_stage::invariants);
  CHECK(none[0].severity == severity::error);
  CHECK(none[0].message.contains("provenance_incomplete"));

  for (std::size_t i = 0; i < field_names.size(); ++i) {
    CAPTURE(field_names[i]);
    const fixed_provenance_source partial(without(i));
    const auto ds = judge(create_cooked(), partial);
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].rule == "R-PRV-INCOMPLETE");
    CHECK(ds[0].message.contains(std::string("'") + std::string(field_names[i]) + "'"));
  }

  const fixed_provenance_source whole(complete());
  CHECK(judge(create_cooked(), whole).empty());
}

TEST_CASE("FAB-INV-PRV-01: a per-op override replaces the origin for its op") {
  // A complete override rescues an incomplete origin.
  fixed_provenance_source rescued(without(2));
  rescued.set_override(0, complete());
  CHECK(judge(create_cooked(), rescued).empty());

  // An incomplete override is not rescued by a complete origin.
  fixed_provenance_source broken(complete());
  broken.set_override(0, without(4));
  const auto ds = judge(create_cooked(), broken);
  REQUIRE(ds.size() == 1);
  CHECK(ds[0].message.contains("'environment_hash'"));

  // An override for another op does not touch this one.
  fixed_provenance_source elsewhere;
  elsewhere.set_override(5, complete());
  CHECK(judge(create_cooked(), elsewhere).size() == 1);
}

TEST_CASE("FAB-INV-PRV-01: records that are not derived need no provenance") {
  const fixed_provenance_source nothing;
  patch p;
  p.ops.push_back(op_create(mesh_id, type_id_of(tg::test_types::mesh),
                            must(value::struct_({field{.no = 1, .v = must(value::string("m"))}}))));
  p.ops.push_back(op_annotate(must(value::string("note"))));
  CHECK(judge(p, nothing).empty());
}

TEST_CASE("FAB-INV-PRV-01: an edit of an existing derived record needs provenance, a delete does "
          "not") {
  tg::oracle_store base;
  base.put(cooked_id,
           record_state{.type = type_id_of(tg::test_types::cooked_mesh), .body = cooked_body()});
  const fixed_provenance_source nothing;
  patch edit;
  edit.ops.push_back(op_set(cooked_id, path{path_segment::field(2)}, value::id(mesh_id)));
  CHECK(judge(edit, nothing, base).size() == 1);
  const fixed_provenance_source whole(complete());
  CHECK(judge(edit, whole, base).empty());

  patch del;
  del.ops.push_back(op_delete(cooked_id));
  CHECK(judge(del, nothing, base).empty());
}

TEST_CASE("FAB-INV-PRV-01: the validator sits in the invariants stage under a unique name") {
  const fixed_provenance_source source;
  const actor_directory directory;
  const domain_registry domains = make_domains();
  const provenance_validator v(source, directory, domains);
  CHECK(v.name() == "fabric.provenance");
  CHECK(v.stage() == validator_stage::invariants);
  validator_registry reg;
  must_ok(reg.add(std::make_shared<provenance_validator>(source, directory, domains)));
  CHECK(reg.size() == 1);
}
