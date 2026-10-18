// FAB-INV-ID-04 (SL-ARCH-001 ARCH1-R01): a typed_id made from an id of another class is
// id_class_mismatch, and assigning or converting between classes does not compile.

#include "sl/fabric/id.hpp"

#include <doctest/doctest.h>

#include <string>
#include <type_traits>

namespace {

using sl::fabric::asset_id;
using sl::fabric::entity_id;
using sl::fabric::id_class;
using sl::fabric::record_id;

} // namespace

// Compile-time half of the invariant. The requires-expressions sit in variable templates so a
// failing requirement makes them false instead of ill-formed.
template <class To, class From>
constexpr bool assignable = requires(To to, From from) { to = from; };
template <class To, class From>
constexpr bool brace_constructible = requires(From from) { To{from}; };
template <class A, class B> constexpr bool comparable = requires(A a, B b) { a == b; };

static_assert(assignable<asset_id, asset_id>);
static_assert(comparable<asset_id, asset_id>);
static_assert(!assignable<asset_id, entity_id>);
static_assert(!assignable<entity_id, asset_id>);
static_assert(!assignable<asset_id, record_id>);
static_assert(!assignable<record_id, asset_id>);
static_assert(!brace_constructible<asset_id, record_id>);
static_assert(!brace_constructible<asset_id, entity_id>);
static_assert(!comparable<asset_id, entity_id>);
static_assert(!std::is_constructible_v<asset_id, entity_id>);
static_assert(!std::is_constructible_v<asset_id, record_id>);
static_assert(!std::is_convertible_v<entity_id, asset_id>);
static_assert(!std::is_convertible_v<asset_id, record_id>);
static_assert(std::is_same_v<decltype(std::declval<asset_id>().id()), const record_id &>);
static_assert(asset_id::klass == id_class::asset);
static_assert(sl::fabric::import_id::klass == id_class::import);
static_assert(sl::fabric::type_id::klass == id_class::type);
static_assert(std::is_trivially_copyable_v<asset_id>);
static_assert(sizeof(asset_id) == record_id::size);

TEST_CASE("FAB-INV-ID-04: typed_id::from checks the class") {
  const record_id ent = sl::fabric::well_known(id_class::entity, 1);
  const auto ok = entity_id::from(ent);
  REQUIRE(ok.has_value());
  CHECK(ok->id() == ent);
  CHECK(ok->to_string() == ent.to_string());

  const auto bad = asset_id::from(ent);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::id_class_mismatch);
  CHECK(bad.error().message.find("expected a asset id") != std::string::npos);
  CHECK(bad.error().message.find(ent.to_string()) != std::string::npos);

  const auto null = asset_id::from(record_id{});
  REQUIRE_FALSE(null.has_value());
  CHECK(null.error().code == sl::errc::id_class_mismatch);
  CHECK(asset_id{}.is_null());
}

TEST_CASE("FAB-INV-ID-04: typed_id::parse reports text errors and class mismatches") {
  const std::string ent_text = sl::fabric::well_known(id_class::entity, 4).to_string();
  CHECK(entity_id::parse(ent_text).has_value());
  CHECK(asset_id::parse(ent_text).error().code == sl::errc::id_class_mismatch);
  CHECK(asset_id::parse("ast_bad").error().code == sl::errc::id_parse);
  const auto a = asset_id::parse(sl::fabric::well_known(id_class::asset, 4).to_string());
  const auto b = asset_id::parse(sl::fabric::well_known(id_class::asset, 5).to_string());
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(*a < *b);
  CHECK(*a != *b);
}
