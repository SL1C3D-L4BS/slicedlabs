// RT-INV-REFL-06 (SL-ARCH-003 ARCH3-R136, `seams` V06-5): the registry dump is one JSON object per
// type and line, sorted by name, byte-identical across runs and registration orders, with a fixed
// byte format (pinned below) and JSON escapes for names.

#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rt = sl::runtime;
using rt::field_flags;
using rt::field_kind;

struct small {
  std::uint32_t id = 0;
  float speed = 0.0F;
  bool on = false;
};

sl::result<rt::method_arg> nop(void * /*obj*/, std::span<const rt::method_arg> /*args*/) noexcept {
  return rt::method_arg{false};
}

constexpr std::array<field_kind, 2> nudge_params{field_kind::f32, field_kind::entity};

rt::type_info small_info(std::string_view name) {
  rt::type_builder<small> b(name);
  SL_RT_FIELD(b, small, id, field_flags::snapshot | field_flags::hashed);
  SL_RT_FIELD(b, small, speed, field_flags::snapshot);
  SL_RT_FIELD(b, small, on, field_flags::transient);
  b.method("nudge", nudge_params, &nop);
  return b.build();
}

std::string dump_of(const std::vector<std::string_view> &order) {
  rt::type_registry reg;
  for (const std::string_view name : order) {
    REQUIRE(reg.add(small_info(name)).has_value());
  }
  REQUIRE(reg.freeze().has_value());
  return reg.dump();
}

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    REQUIRE(end != std::string::npos);
    out.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::string name_of(const std::string &line) {
  const std::string_view prefix = R"({"name":")";
  REQUIRE(line.starts_with(prefix));
  const std::size_t end = line.find('"', prefix.size());
  return line.substr(prefix.size(), end - prefix.size());
}

} // namespace

TEST_CASE("RT-INV-REFL-06: the dump is sorted by name, one object per line") {
  const std::string d = dump_of({"zeta", "alpha", "Mid", "beta.two", "beta"});
  const std::vector<std::string> ls = lines(d);
  REQUIRE(ls.size() == 5);
  const std::vector<std::string_view> want{"Mid", "alpha", "beta", "beta.two", "zeta"};
  for (std::size_t i = 0; i < want.size(); ++i) {
    CHECK(name_of(ls[i]) == want[i]);
    CHECK(ls[i].ends_with('}'));
  }
}

TEST_CASE("RT-INV-REFL-06: the dump is byte-identical across runs and registration orders") {
  const std::string a = dump_of({"zeta", "alpha", "Mid"});
  const std::string b = dump_of({"Mid", "zeta", "alpha"});
  const std::string c = dump_of({"alpha", "Mid", "zeta"});
  CHECK(a == b);
  CHECK(a == c);
  CHECK(dump_of({"zeta", "alpha", "Mid"}) == a);
  CHECK(dump_of({}).empty());
}

TEST_CASE("RT-INV-REFL-06: the byte format is pinned") {
  const std::string d = dump_of({"a"});
  const std::string want =
      "{\"name\":\"a\",\"type_key\":\"0xaf63dc4c8601ec8c\",\"size\":12,\"align\":4,"
      "\"schema_type\":null,\"fields\":["
      "{\"name\":\"id\",\"offset\":0,\"size\":4,\"kind\":\"u32\",\"flags\":[\"snapshot\","
      "\"hashed\"],\"fabric_field\":0},"
      "{\"name\":\"speed\",\"offset\":4,\"size\":4,\"kind\":\"f32\",\"flags\":[\"snapshot\"],"
      "\"fabric_field\":0},"
      "{\"name\":\"on\",\"offset\":8,\"size\":1,\"kind\":\"bool\",\"flags\":[\"transient\"],"
      "\"fabric_field\":0}],"
      "\"methods\":[{\"name\":\"nudge\",\"params\":[\"f32\",\"entity\"]}]}\n";
  CHECK(d == want);
}

TEST_CASE("RT-INV-REFL-06: names are JSON-escaped") {
  rt::type_registry reg;
  rt::type_builder<small> b("q\"uote\\back\x01");
  b.field("tab\there", 0, field_kind::u32, field_flags::none);
  REQUIRE(reg.add(b.build()).has_value());
  const std::string d = reg.dump();
  CHECK(d.find("\"name\":\"q\\\"uote\\\\back\\u0001\"") != std::string::npos);
  CHECK(d.find("\"name\":\"tab\\there\"") != std::string::npos);
  CHECK(d.find("\"flags\":[]") != std::string::npos);
}
