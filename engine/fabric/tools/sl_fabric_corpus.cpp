// sl_fabric_corpus (WO-041): the C++ side of `just fabric-determinism` (FAB-INV-SER-04).
//
//   sl_fabric_corpus <accept.json> <out.json>
//
// Reads the SLB/1 accept corpus (tests/fixtures/fabric/slb/accept.json) and the corpus schemas
// next to it (schemas.json), encodes every vector's value with the C++ codec, checks that the
// bytes decode and re-encode to themselves, and writes one row per vector in corpus order:
//
//   {"format": "sl-fabric-corpus/1", "vectors": [{"name", "hex", "oid", "text"?}...]}
//
// `text` is the canonical text of the vector's type, present when the corpus type has a C++ type
// (load_corpus_schemas). tools/fabric/determinism.py compares the clang and gcc outputs byte for
// byte and both against accept.json. Exit 0 on success, 1 on a codec failure, 2 on bad input.

#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace {

using namespace sl::fabric;

std::optional<std::string> read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::optional<json_value> read_json(const std::string &path) {
  const auto text = read_file(path);
  if (!text) {
    std::println(stderr, "sl_fabric_corpus: cannot read {}", path);
    return std::nullopt;
  }
  auto doc = parse_json(*text);
  if (!doc) {
    std::println(stderr, "sl_fabric_corpus: {}: {}", path, doc.error().message);
    return std::nullopt;
  }
  return std::move(*doc);
}

/// A JSON string literal of `s`: the canonical text of a string value is exactly that.
std::string quoted(std::string_view s, const schema_registry &builtins) {
  auto v = value::string(s);
  auto t = v ? to_text(*v, type_id_of("string"), builtins) : std::unexpected(v.error());
  return t ? *t : std::string("\"?\"");
}

int fail(std::string_view name, const sl::error &e) {
  std::println(stderr, "sl_fabric_corpus: vector '{}': {}", name, e.message);
  return 1;
}

int run(const std::string &accept_path, const std::string &out_path) {
  const std::size_t slash = accept_path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : accept_path.substr(0, slash);
  const auto accept = read_json(accept_path);
  const auto schemas_doc = read_json(dir + "/schemas.json");
  if (!accept || !schemas_doc) {
    return 2;
  }
  const auto schemas = load_corpus_schemas(*schemas_doc);
  const json_value *vectors = accept->find("vectors");
  if (!schemas || vectors == nullptr) {
    std::println(stderr, "sl_fabric_corpus: bad corpus files in {}", dir);
    return 2;
  }
  const schema_registry builtins;
  std::string out = "{\n\"format\": \"sl-fabric-corpus/1\",\n\"vectors\": [\n";
  std::size_t texts = 0;
  for (std::size_t i = 0; i < vectors->items.size(); ++i) {
    const json_value &vec = vectors->items[i];
    const json_value *name = vec.find("name");
    const json_value *notation = vec.find("value");
    const json_value *type = vec.find("type");
    if (name == nullptr || notation == nullptr || type == nullptr) {
      std::println(stderr, "sl_fabric_corpus: vector {} lacks name, value or type", i);
      return 2;
    }
    const auto v = value_from_notation(*notation);
    if (!v) {
      return fail(name->text, v.error());
    }
    const auto bytes = encode(*v);
    if (!bytes) {
      return fail(name->text, bytes.error());
    }
    const auto back = decode(*bytes);
    const auto again = back ? encode(*back) : std::unexpected(back.error());
    if (!again) {
      return fail(name->text, again.error());
    }
    if (*again != *bytes) {
      std::println(stderr, "sl_fabric_corpus: vector '{}': decode and re-encode changed the bytes",
                   name->text);
      return 1;
    }
    out += std::format(R"({{"name": {}, "hex": "{}", "oid": "sha256:{}")",
                       quoted(name->text, builtins), to_hex(*bytes),
                       record_object_id(*bytes).to_hex());
    const auto id = schemas->types.find(type->text);
    if (type->kind == json_value::kind_type::string && id != schemas->types.end()) {
      const auto text = to_text(*v, id->second, schemas->registry);
      if (!text) {
        return fail(name->text, text.error());
      }
      out += R"(, "text": )" + quoted(*text, builtins);
      ++texts;
    }
    out += i + 1 < vectors->items.size() ? "},\n" : "}\n";
  }
  out += "]\n}\n";
  std::ofstream file(out_path, std::ios::binary | std::ios::trunc);
  file << out;
  file.close();
  if (!file) {
    std::println(stderr, "sl_fabric_corpus: cannot write {}", out_path);
    return 2;
  }
  std::println("sl_fabric_corpus: {} vectors ({} with text) -> {}", vectors->items.size(), texts,
               out_path);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    if (args.size() != 3) {
      std::println(stderr, "usage: sl_fabric_corpus <accept.json> <out.json>");
      return 2;
    }
    return run(args[1], args[2]);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_corpus: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_corpus: unknown error\n", stderr);
  }
  return 1;
}
