// FAB-INV-GEN-02 (SL-ARCH-001 ARCH1-R67, ADR-0013): every generated typed struct round trips
// through from_value and to_value on its fixtures
// (tests/fixtures/fabric/types/<module>/<type>.json, canonical text), the generated descriptors
// register and freeze in one registry, typed values built in C++ pass the registry, and from_value
// refuses what the generated validator tables refuse.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/gen/fabric.edge.gen.hpp"
#include "sl/fabric/gen/fabric.event.gen.hpp"
#include "sl/fabric/gen/fabric.manifest.gen.hpp"
#include "sl/fabric/gen/system.gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <fstream>
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

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return {};
  }
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

/// tests/fixtures/fabric/types/<module>/<local>.json, the module being the longest prefix with a
/// fixture directory.
json_value load_fixture(std::string_view type_name) {
  for (std::size_t dot = type_name.rfind('.'); dot != std::string_view::npos && dot > 0;
       dot = type_name.rfind('.', dot - 1)) {
    const std::string path = std::format("{}/types/{}/{}.json", SL_FABRIC_FIXTURES_DIR,
                                         type_name.substr(0, dot), type_name.substr(dot + 1));
    const std::string text = read_file(path);
    if (!text.empty()) {
      return must(parse_json(text));
    }
  }
  FAIL("no fixture file for " << type_name);
  return {};
}

void write_string(std::string &out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        out += std::format("\\u{:04x}", c);
      } else {
        out += ch;
      }
    }
  }
  out += '"';
}

/// Compact JSON with members in document order and number tokens as written: the canonical text
/// of a fixture value (slb_text rules).
void write_compact(std::string &out, const json_value &j) {
  switch (j.kind) {
  case json_value::kind_type::null:
    out += "null";
    break;
  case json_value::kind_type::boolean:
    out += j.boolean ? "true" : "false";
    break;
  case json_value::kind_type::number:
    out += j.text;
    break;
  case json_value::kind_type::string:
    write_string(out, j.text);
    break;
  case json_value::kind_type::array:
    out += '[';
    for (std::size_t i = 0; i < j.items.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_compact(out, j.items[i]);
    }
    out += ']';
    break;
  case json_value::kind_type::object:
    out += '{';
    for (std::size_t i = 0; i < j.members.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_string(out, j.members[i].key);
      out += ':';
      write_compact(out, j.members[i].v);
    }
    out += '}';
    break;
  }
}

std::string compact(const json_value &j) {
  std::string out;
  write_compact(out, j);
  return out;
}

record_id rid(id_class c, std::string_view key) {
  return derive(c, well_known(id_class::namespace_, 1), key);
}

digest dig(std::string_view text) {
  return digest{.algo = hash_algo::sha256, .bytes = sha256::hash(text)};
}

void check_conforms(const schema_registry &reg, const value &v, std::string_view type) {
  std::vector<diagnostic_lite> diags;
  const auto r = reg.check(v, type_id_of(type), diags);
  CHECK_MESSAGE(r.has_value(), (diags.empty() ? std::string(type) : diags.front().message));
}

} // namespace

TEST_CASE("FAB-INV-GEN-02: every generated type round trips its fixtures") {
  const schema_registry reg = must(gen::system_registry());
  const std::vector<gen::type_entry> types = gen::system_types();
  REQUIRE_FALSE(types.empty());
  std::size_t values = 0;
  for (const gen::type_entry &t : types) {
    CAPTURE(t.name);
    REQUIRE(t.roundtrip != nullptr);
    REQUIRE(reg.find(t.name) != nullptr);
    const json_value doc = load_fixture(t.name);
    const json_value *items = doc.find("values");
    REQUIRE(items != nullptr);
    CHECK(items->items.size() >= 2);
    for (const json_value &item : items->items) {
      const std::string text = compact(item);
      CAPTURE(text);
      const value parsed = must(from_text(text, type_id_of(t.name), reg));
      check_conforms(reg, parsed, t.name);
      const value back = must(t.roundtrip(parsed));
      CHECK(back == parsed);
      CHECK(must(encode(back)) == must(encode(parsed)));
      CHECK(must(to_text(back, type_id_of(t.name), reg)) == text);
      ++values;
    }
  }
  CHECK(values >= 2 * types.size());
}

TEST_CASE("FAB-INV-GEN-02: typed values built in C++ conform and round trip") {
  const schema_registry reg = must(gen::system_registry());

  const gen::fabric_edge::edge e{.from = rid(id_class::entity, "test"),
                                 .to = rid(id_class::asset, "crate"),
                                 .kind = gen::fabric_edge::edge_kind::validates};
  const value ev = must(e.to_value());
  check_conforms(reg, ev, "fabric.edge.edge");
  CHECK(must(gen::fabric_edge::edge::from_value(ev)) == e);
  // The edge record is the layout the dependency graph reads (WO-044, domain fabric.edge).
  CHECK(reg.find("fabric.edge.edge")->domain == "fabric.edge");
  const edge read = must(edge_from_record(ev));
  CHECK(read.from == e.from);
  CHECK(read.to == e.to);
  CHECK(read.kind == edge_kind::validates);

  gen::fabric_event::about a;
  a.alt = std::string("workspace");
  const value av = must(a.to_value());
  check_conforms(reg, av, "fabric.event.about");
  CHECK(must(gen::fabric_event::about::from_value(av)) == a);
  a.alt = rid(id_class::entity, "subject");
  CHECK(must(gen::fabric_event::about::from_value(must(a.to_value()))) == a);

  gen::fabric_manifest::manifest m;
  m.project = rid(id_class::project, "demo");
  m.name = "Demo";
  m.writer_format = {.major = 1, .minor = 2};
  m.min_reader_format = {.major = 1, .minor = 0};
  m.schema_set = dig("schema set");
  m.modules["sim"] = {.at_least = {.major = 1, .minor = 0}, .below = {.major = 2, .minor = 0}};
  m.modules["fabric"] = {.at_least = {.major = 1, .minor = 1}, .below = {.major = 2, .minor = 0}};
  m.capabilities = {"cap.render.vulkan"};
  m.package_pins.push_back({.element = rid(id_class::element, "pin"),
                            .order_key = {0x80},
                            .v = {.package = rid(id_class::package, "core"),
                                  .revision = dig("core"),
                                  .mount = "packages/core"}});
  m.default_branch = "main";
  const value mv = must(m.to_value());
  check_conforms(reg, mv, "fabric.manifest.manifest");
  CHECK(must(gen::fabric_manifest::manifest::from_value(mv)) == m);
  // Map entries are canonical by encoded key: "sim" (3 bytes) sorts before "fabric" (6 bytes).
  CHECK(must(to_text(mv.find_field(6) != nullptr ? *mv.find_field(6) : value{},
                     type_id_of("fabric.manifest.module_ranges"), reg))
            .starts_with("[[\"sim\""));
}

TEST_CASE("FAB-INV-GEN-02: from_value refuses what the validator tables refuse") {
  using gen::fabric_edge::edge;
  const auto refused = [](const auto &r, std::string_view needle) {
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::type_mismatch);
    CHECK_MESSAGE(r.error().message.find(needle) != std::string::npos, r.error().message);
  };
  const value from = value::id(rid(id_class::entity, "a"));
  const value to = value::id(rid(id_class::entity, "b"));

  refused(edge::from_value(value::uint(1)), "expected a struct");
  refused(edge::from_value(
              must(value::struct_({{.no = 1, .v = from}, {.no = 3, .v = value::uint(4)}}))),
          "field 2: required field is absent");
  refused(edge::from_value(must(value::struct_({{.no = 1, .v = from},
                                                {.no = 2, .v = to},
                                                {.no = 3, .v = value::uint(4)},
                                                {.no = 9, .v = value::uint(0)}}))),
          "field 9 is not declared");
  refused(edge::from_value(must(value::struct_(
              {{.no = 1, .v = from}, {.no = 2, .v = to}, {.no = 3, .v = value::uint(9)}}))),
          "9 is not an enumerator");
  refused(
      edge::from_value(must(value::struct_(
          {{.no = 1, .v = from}, {.no = 2, .v = value::uint(2)}, {.no = 3, .v = value::uint(4)}}))),
      "expected id");
  refused(
      gen::fabric_manifest::format_version::from_value(must(value::struct_(
          {{.no = 1, .v = value::uint(std::uint64_t{1} << 33U)}, {.no = 2, .v = value::uint(0)}}))),
      "unsigned integer in range");
  refused(gen::fabric_event::time_us_from_value(value::uint(std::uint64_t{1} << 63U)),
          "signed integer in range");
  refused(gen::fabric_event::about::from_value(must(value::variant(7, value::uint(0)))),
          "7 is not a case");
  refused(gen::fabric_event::about::from_value(must(value::variant(1, value::uint(0)))),
          "expected id");
  // Encoding refuses a typed value whose ids were never set.
  refused(edge{}.to_value(), "null id");
}
