// sl_fabric_dump_contract (WO-062): the C++ side of `just fabric-contract-lock` (FAB-INV-CF-03).
//
//   sl_fabric_dump_contract <out.json | ->
//
// Writes the contract tables as the engine compiles them, read from the code that defines them
// (no copy of the documents in here):
//
//   {"format": "sl-fabric-contract/1",
//    "id_classes": [{class, name, prefix}...], "mint_schemes": [{scheme, name}...],
//    "type_kinds": [name...], "slb_tags": [{tag, name}...],
//    "limits": {depth, items, record_body_bytes, loose_blob_threshold_bytes},
//    "epistemic": {classes: [...], transitions: [{from, to}...]},
//    "error_codes": [[name, number]...], "corruption": [{class, name, detect, policy,
//    evidence}...], "failpoints": [{fp, at, outcome}...], "tx_states": [name...]}
//
// tools/fabric/contract_lock.py compares it with the `sl-data` tables of SL-ARCH-001 and
// SL-ARCH-002 and with schemas/fabric/contract.lock.json. Exit 0 on success, 2 on bad usage or a
// write failure.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace {

using namespace sl::fabric;

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += std::format("\\u{:04x}", static_cast<unsigned>(c));
      } else {
        out += c;
      }
    }
  }
  return out + "\"";
}

// Appends ",\n    " between items of one list.
void sep(std::string &out, bool &first) {
  out += first ? "\n    " : ",\n    ";
  first = false;
}

std::string id_classes() {
  std::string out = "[";
  bool first = true;
  for (std::uint8_t n = 1; n <= 15; ++n) {
    const auto c = static_cast<id_class>(n);
    sep(out, first);
    out += std::format(R"({{"class": {}, "name": {}, "prefix": {}}})", n, quote(to_string(c)),
                       quote(prefix(c)));
  }
  return out + "\n  ]";
}

std::string mint_schemes() {
  std::string out = "[";
  bool first = true;
  for (std::uint8_t n = 1; n <= 4; ++n) {
    sep(out, first);
    out += std::format(R"({{"scheme": {}, "name": {}}})", n,
                       quote(to_string(static_cast<mint_scheme>(n))));
  }
  return out + "\n  ]";
}

std::string type_kinds() {
  std::string out = "[";
  bool first = true;
  for (std::uint8_t n = 1; to_string(static_cast<type_kind>(n)) != "unknown"; ++n) {
    sep(out, first);
    out += quote(to_string(static_cast<type_kind>(n)));
  }
  return out + "\n  ]";
}

// The SLB/1 tags: value_kind numbers are the tags, and bool covers 0x01 false and 0x02 true.
std::string slb_tags() {
  std::string out = "[";
  bool first = true;
  sep(out, first);
  out += R"({"tag": "0x01", "name": "false"})";
  sep(out, first);
  out += R"({"tag": "0x02", "name": "true"})";
  for (std::uint8_t n = 3; n <= 15; ++n) {
    sep(out, first);
    out += std::format(R"({{"tag": "0x{:02X}", "name": {}}})", n,
                       quote(to_string(static_cast<value_kind>(n))));
  }
  return out + "\n  ]";
}

std::string epistemic() {
  std::string out = R"({"classes": [)";
  for (std::uint32_t n = 1; n <= 6; ++n) {
    out +=
        std::format("{}{}", n == 1 ? "" : ", ", quote(to_string(static_cast<epistemic_class>(n))));
  }
  out += R"(], "transitions": [)";
  bool first = true;
  for (const class_transition &t : legal_transitions) {
    out += std::format(R"({}{{"from": {}, "to": {}}})", first ? "" : ", ", quote(to_string(t.from)),
                       quote(to_string(t.to)));
    first = false;
  }
  return out + "]}";
}

// Every fabric code (5000-5999) the error domain names.
std::string error_codes() {
  std::string out = "[";
  bool first = true;
  for (int n = 5000; n <= 5999; ++n) {
    const std::string_view name = sl::to_string(static_cast<sl::errc>(n));
    if (name == "unknown") {
      continue;
    }
    sep(out, first);
    out += std::format("[{}, {}]", quote(name), n);
  }
  return out + "\n  ]";
}

std::string corruption() {
  std::string out = "[";
  bool first = true;
  for (const corruption_policy &p : corruption_policies()) {
    sep(out, first);
    out +=
        std::format(R"({{"class": "C{}", "name": {}, "detect": {}, "policy": {}, "evidence": {}}})",
                    static_cast<unsigned>(p.cls), quote(p.name), quote(p.detect), quote(p.policy),
                    quote(p.evidence));
  }
  return out + "\n  ]";
}

std::string failpoints() {
  std::string out = "[";
  bool first = true;
  for (const failpoint_info &f : failpoint_table()) {
    sep(out, first);
    out += std::format(R"({{"fp": {}, "at": {}, "outcome": {}}})", quote(f.name), quote(f.at),
                       quote(f.outcome));
  }
  return out + "\n  ]";
}

std::string tx_states() {
  std::string out = "[";
  for (std::uint8_t n = 1; n <= 9; ++n) {
    out += std::format("{}{}", n == 1 ? "" : ", ", quote(to_string(static_cast<tx_state>(n))));
  }
  return out + "]";
}

std::string contract() {
  std::string out = "{\n  \"format\": \"sl-fabric-contract/1\",\n";
  out += "  \"id_classes\": " + id_classes() + ",\n";
  out += "  \"mint_schemes\": " + mint_schemes() + ",\n";
  out += "  \"type_kinds\": " + type_kinds() + ",\n";
  out += "  \"slb_tags\": " + slb_tags() + ",\n";
  out += std::format("  \"limits\": {{\"depth\": {}, \"items\": {}, \"record_body_bytes\": {}, "
                     "\"loose_blob_threshold_bytes\": {}}},\n",
                     max_value_depth, max_container_items, max_record_body, loose_object_threshold);
  out += "  \"epistemic\": " + epistemic() + ",\n";
  out += "  \"error_codes\": " + error_codes() + ",\n";
  out += "  \"corruption\": " + corruption() + ",\n";
  out += "  \"failpoints\": " + failpoints() + ",\n";
  out += "  \"tx_states\": " + tx_states() + "\n}\n";
  return out;
}

int run(std::span<char *> args) {
  if (args.size() != 2) {
    std::fputs("usage: sl_fabric_dump_contract <out.json | ->\n", stderr);
    return 2;
  }
  const std::string text = contract();
  const std::string_view out = args[1];
  if (out == "-") {
    std::fputs(text.c_str(), stdout);
    return 0;
  }
  std::ofstream f{std::string(out), std::ios::binary | std::ios::trunc};
  f << text;
  f.close();
  if (!f) {
    std::fputs(std::format("sl_fabric_dump_contract: cannot write {}\n", out).c_str(), stderr);
    return 2;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    return run(args);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_dump_contract: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_dump_contract: unknown error\n", stderr);
  }
  return 2;
}
