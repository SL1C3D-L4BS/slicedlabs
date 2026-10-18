// FAB-INV-SCH-04 (SL-ARCH-001 ARCH1-R21): C++ classify equals the Python classifier on the golden
// table. tests/fixtures/fabric/schema/compat_table.json holds one {old, new, expected} pair per row
// of the SL-ARCH-001 `compatibility` table as tools/fabric/schema_compat.py classifies it (32 rows
// over the 23 table rows); every row is read into type descriptors and classified in C++. The
// migration fixture modules classify as the Python would (identical to themselves, v1 -> v2
// breaking), and check_change demands the step a backward_only or breaking change needs and
// refuses a forbidden one.

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/type.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

json_value read_json(std::string_view rel) {
  const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/" + std::string(rel);
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return must(parse_json(s.str()));
}

const type_desc &named(const std::vector<type_desc> &types, std::string_view name) {
  for (const type_desc &t : types) {
    if (t.name == name) {
      return t;
    }
  }
  FAIL("no type " << name);
  return types.front();
}

} // namespace

TEST_CASE("FAB-INV-SCH-04: C++ classify equals the Python classifier on the golden table") {
  const json_value table = read_json("schema/compat_table.json");
  REQUIRE(table.find("format") != nullptr);
  CHECK(table.find("format")->text == "sl-compat-table/1");
  const json_value *rows = table.find("rows");
  REQUIRE(rows != nullptr);
  REQUIRE(rows->items.size() == 32);
  std::set<std::string> changes;
  for (const json_value &row : rows->items) {
    const std::string name = row.find("case")->text;
    CAPTURE(name);
    const type_desc from = must(type_from_json(*row.find("old")));
    const type_desc to = must(type_from_json(*row.find("new")));
    CHECK(to_string(classify(from, to)) == row.find("expected")->text);
    CHECK(classify(from, from) == compat::identical);
    changes.insert(row.find("change")->text);
  }
  CHECK(changes.size() == 23); // every row of the SL-ARCH-001 table is covered
}

TEST_CASE("FAB-INV-SCH-04: the migration modules classify as the Python does; check_change "
          "demands the step") {
  const auto v1 = must(types_from_module(read_json("migration/content.demo.v1.sltype.json")));
  const auto v2 = must(types_from_module(read_json("migration/content.demo.v2.sltype.json")));
  for (const auto *types : {&v1, &v2}) {
    for (const type_desc &t : *types) {
      CAPTURE(t.name);
      CHECK(classify(t, t) == compat::identical);
    }
  }
  const type_desc &item1 = named(v1, "content.demo.item");
  const type_desc &item2 = named(v2, "content.demo.item");
  const type_desc &note1 = named(v1, "content.demo.note");
  CHECK(classify(item1, item2) == compat::breaking); // add required tag
  CHECK(classify(note1, named(v2, "content.demo.note")) == compat::breaking);
  CHECK(classify(item1, note1) == compat::forbidden); // two different types

  // Single changes on the v1 item.
  type_desc widened = item1;
  widened.fields[1].type = type_id_of("u32"); // count u16 -> u32
  CHECK(classify(item1, widened) == compat::backward_only);
  type_desc narrowed = item1;
  narrowed.fields[1].type = type_id_of("u8");
  CHECK(classify(item1, narrowed) == compat::breaking);
  type_desc relabelled = item1;
  relabelled.fields[0].name = "title";
  relabelled.fields[0].doc = "renamed";
  relabelled.fields[2].deprecated_in = 1;
  CHECK(classify(item1, relabelled) == compat::identical);
  type_desc moved = item1;
  moved.domain = "content.other";
  CHECK(classify(item1, moved) == compat::forbidden);

  // check_change (ARCH1-R21): backward_only and breaking need a hop; forbidden is refused.
  migration_registry none;
  CHECK(must(none.check_change(item1, relabelled)) == compat::identical);
  auto missing = none.check_change(item1, item2);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == errc::migration_missing_step);
  CHECK(none.check_change(item1, moved).error().code == errc::schema_invalid);
  migration_registry with_step;
  must_ok(with_step.add(migration_step{.type = item1.id,
                                       .from = item1.version,
                                       .to = item2.version,
                                       .kind = step_kind::identity,
                                       .field = 0,
                                       .fill = std::nullopt,
                                       .converter = "",
                                       .loss = ""}));
  CHECK(must(with_step.check_change(item1, item2)) == compat::breaking);
  CHECK(to_string(compat::backward_only) == "backward_only");
}
