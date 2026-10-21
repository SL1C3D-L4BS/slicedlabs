// RT-INV-REFL-04 (SL-ARCH-003 ARCH3-R133, `limits` max_method_arity, `error_codes` reflection):
// invoking a method with a wrong argument count, kind or range is reflect_mismatch, unknown type
// and method names are type_unknown, unknown field names field_unknown, and nothing throws: every
// lookup and invoke is noexcept and invokers are noexcept by type.

#include "sl/core/error.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
using rt::field_flags;
using rt::field_kind;
using rt::method_arg;

struct counter {
  std::int64_t value = 0;
  float scale = 1.0F;
  bool flag = false;
  rt::entity_bits target;
};

counter &self(void *obj) noexcept { return *static_cast<counter *>(obj); }

/// The argument's value (invoke() checked the alternative); a default value otherwise.
template <class A> A arg(const method_arg &a) noexcept {
  const A *v = std::get_if<A>(&a);
  return v != nullptr ? *v : A{};
}

sl::result<method_arg> add_i32(void *obj, std::span<const method_arg> args) noexcept {
  self(obj).value += arg<std::int64_t>(args[0]);
  return method_arg{self(obj).value};
}

sl::result<method_arg> set_scale(void *obj, std::span<const method_arg> args) noexcept {
  self(obj).scale = static_cast<float>(arg<double>(args[0]));
  return method_arg{true};
}

sl::result<method_arg> set_flag(void *obj, std::span<const method_arg> args) noexcept {
  self(obj).flag = arg<bool>(args[0]);
  return method_arg{self(obj).flag};
}

sl::result<method_arg> aim(void *obj, std::span<const method_arg> args) noexcept {
  self(obj).target = arg<rt::entity_bits>(args[0]);
  return method_arg{self(obj).target};
}

sl::result<method_arg> sum4(void *obj, std::span<const method_arg> args) noexcept {
  const std::uint64_t s = arg<std::uint64_t>(args[0]) + arg<std::uint64_t>(args[1]) +
                          arg<std::uint64_t>(args[2]) + arg<std::uint64_t>(args[3]);
  self(obj).value = static_cast<std::int64_t>(s);
  return method_arg{s};
}

sl::result<method_arg> reset(void *obj, std::span<const method_arg> /*args*/) noexcept {
  self(obj) = counter{};
  return method_arg{std::int64_t{0}};
}

constexpr std::array<field_kind, 1> add_params{field_kind::i32};
constexpr std::array<field_kind, 1> scale_params{field_kind::f32};
constexpr std::array<field_kind, 1> flag_params{field_kind::boolean};
constexpr std::array<field_kind, 1> aim_params{field_kind::entity};
constexpr std::array<field_kind, 4> sum_params{field_kind::u8, field_kind::u16, field_kind::u32,
                                               field_kind::u64};
constexpr std::array<field_kind, 5> five_params{field_kind::u8, field_kind::u8, field_kind::u8,
                                                field_kind::u8, field_kind::u8};
constexpr std::array<field_kind, 1> id_params{field_kind::record_id};

rt::type_info counter_info() {
  rt::type_builder<counter> b("test.refl.counter");
  SL_RT_FIELD(b, counter, value, field_flags::hashed);
  SL_RT_FIELD(b, counter, scale, field_flags::snapshot);
  SL_RT_FIELD(b, counter, flag, field_flags::transient);
  SL_RT_FIELD(b, counter, target, field_flags::snapshot);
  b.method("add", add_params, &add_i32)
      .method("scale", scale_params, &set_scale)
      .method("flag", flag_params, &set_flag)
      .method("aim", aim_params, &aim)
      .method("sum", sum_params, &sum4)
      .method("reset", {}, &reset);
  return b.build();
}

bool is(const sl::result<method_arg> &r, sl::errc code) {
  return !r.has_value() && r.error().code == code;
}

struct fixture {
  rt::type_registry reg;
  const rt::type_info *type = nullptr;

  fixture() {
    const auto added = reg.add(counter_info());
    REQUIRE(added.has_value());
    type = *added;
    REQUIRE(reg.freeze().has_value());
  }

  [[nodiscard]] const rt::method_info &method(std::string_view name) const {
    const auto m = rt::type_registry::method(*type, name);
    REQUIRE(m.has_value());
    return **m;
  }
};

} // namespace

TEST_CASE("RT-INV-REFL-04: well-formed calls reach the invoker") {
  const fixture f;
  counter c;
  const std::array<method_arg, 1> five{std::int64_t{5}};
  const auto r = rt::invoke(f.method("add"), &c, five);
  REQUIRE(r.has_value());
  CHECK(std::get<std::int64_t>(*r) == 5);
  CHECK(c.value == 5);
  const std::array<method_arg, 1> half{0.5};
  REQUIRE(rt::invoke(f.method("scale"), &c, half).has_value());
  CHECK(c.scale == 0.5F);
  const std::array<method_arg, 1> yes{true};
  REQUIRE(rt::invoke(f.method("flag"), &c, yes).has_value());
  CHECK(c.flag);
  const std::array<method_arg, 1> e{rt::entity_bits{.index = 3, .generation = 9}};
  REQUIRE(rt::invoke(f.method("aim"), &c, e).has_value());
  CHECK(c.target == rt::entity_bits{.index = 3, .generation = 9});
  const std::array<method_arg, 4> four{std::uint64_t{255}, std::uint64_t{65535}, std::uint64_t{1},
                                       std::uint64_t{1} << 40U};
  const auto s = rt::invoke(f.method("sum"), &c, four);
  REQUIRE(s.has_value());
  CHECK(std::get<std::uint64_t>(*s) == 255U + 65535U + 1U + (std::uint64_t{1} << 40U));
  REQUIRE(rt::invoke(f.method("reset"), &c, {}).has_value());
  CHECK(c.value == 0);
}

TEST_CASE("RT-INV-REFL-04: wrong count, kind or range is reflect_mismatch and calls nothing") {
  const fixture f;
  counter c;
  c.value = 7;
  const std::array<method_arg, 2> two{std::int64_t{1}, std::int64_t{2}};
  CHECK(is(rt::invoke(f.method("add"), &c, two), sl::errc::reflect_mismatch));
  CHECK(is(rt::invoke(f.method("add"), &c, {}), sl::errc::reflect_mismatch));
  const std::array<method_arg, 1> unsigned_arg{std::uint64_t{1}};
  CHECK(is(rt::invoke(f.method("add"), &c, unsigned_arg), sl::errc::reflect_mismatch));
  const std::array<method_arg, 1> too_big{std::int64_t{1'099'511'627'776}};
  CHECK(is(rt::invoke(f.method("add"), &c, too_big), sl::errc::reflect_mismatch));
  const std::array<method_arg, 1> huge{1e300};
  CHECK(is(rt::invoke(f.method("scale"), &c, huge), sl::errc::reflect_mismatch));
  const std::array<method_arg, 1> integer{std::int64_t{1}};
  CHECK(is(rt::invoke(f.method("flag"), &c, integer), sl::errc::reflect_mismatch));
  CHECK(is(rt::invoke(f.method("aim"), &c, integer), sl::errc::reflect_mismatch));
  const std::array<method_arg, 4> wide{std::uint64_t{256}, std::uint64_t{0}, std::uint64_t{0},
                                       std::uint64_t{0}};
  CHECK(is(rt::invoke(f.method("sum"), &c, wide), sl::errc::reflect_mismatch));
  CHECK(is(rt::invoke(f.method("reset"), &c, integer), sl::errc::reflect_mismatch));
  CHECK(c.value == 7);
  const std::array<method_arg, 1> five{std::int64_t{5}};
  CHECK(is(rt::invoke(f.method("add"), nullptr, five), sl::errc::invalid_argument));
}

TEST_CASE("RT-INV-REFL-04: unknown names are type_unknown or field_unknown") {
  const fixture f;
  const auto no_type = f.reg.find("test.refl.missing");
  REQUIRE_FALSE(no_type.has_value());
  CHECK(no_type.error().code == sl::errc::type_unknown);
  const auto no_method = rt::type_registry::method(*f.type, "missing");
  REQUIRE_FALSE(no_method.has_value());
  CHECK(no_method.error().code == sl::errc::type_unknown);
  const auto no_field = rt::type_registry::field(*f.type, "missing");
  REQUIRE_FALSE(no_field.has_value());
  CHECK(no_field.error().code == sl::errc::field_unknown);
  const auto value = rt::type_registry::field(*f.type, "value");
  REQUIRE(value.has_value());
  CHECK((*value)->kind == field_kind::i64);
}

TEST_CASE("RT-INV-REFL-04: nothing throws and malformed methods are refused") {
  const fixture f;
  counter c;
  static_assert(
      std::is_nothrow_invocable_v<rt::method_invoker, void *, std::span<const method_arg>>);
  const rt::method_info &add = f.method("add");
  static_assert(noexcept(rt::invoke(add, &c, {})));
  static_assert(noexcept(f.reg.find("")));
  static_assert(noexcept(rt::type_registry::field(*f.type, "")));
  static_assert(noexcept(rt::type_registry::method(*f.type, "")));
  static_assert(noexcept(f.reg.dump()));

  rt::type_registry reg;
  rt::type_builder<counter> five("test.refl.five");
  five.method("five", five_params, &sum4);
  const auto r5 = reg.add(five.build());
  REQUIRE_FALSE(r5.has_value());
  CHECK(r5.error().code == sl::errc::invalid_argument);
  rt::type_builder<counter> id("test.refl.id");
  id.method("id", id_params, &sum4);
  const auto rid = reg.add(id.build());
  REQUIRE_FALSE(rid.has_value());
  CHECK(rid.error().code == sl::errc::invalid_argument);
  rt::type_builder<counter> null_invoker("test.refl.null");
  null_invoker.method("none", {}, nullptr);
  const auto rnull = reg.add(null_invoker.build());
  REQUIRE_FALSE(rnull.has_value());
  CHECK(rnull.error().code == sl::errc::invalid_argument);
  rt::type_builder<counter> twice("test.refl.twice");
  twice.method("m", {}, &reset).method("m", {}, &reset);
  const auto rtwice = reg.add(twice.build());
  REQUIRE_FALSE(rtwice.has_value());
  CHECK(rtwice.error().code == sl::errc::invalid_argument);
}
