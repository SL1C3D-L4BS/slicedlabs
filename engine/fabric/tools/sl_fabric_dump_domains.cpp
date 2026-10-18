// sl_fabric_dump_domains (WO-048): the C++ side of `just fabric-ownership-check` (F2,
// FAB-INV-OWN-08).
//
//   sl_fabric_dump_domains [--claim <domain>=<owner>]... <out.json | ->
//
// Builds the domain registry the engine starts with (claim_fabric_domains), attempts every extra
// --claim after it (owner: a code "05" or a prefix "content"), and writes the registry as JSON:
//
//   {"format": "sl-fabric-domains/1", "reserved": [{prefix, owner, owns, state_classes}...],
//    "fabric_domains": [...], "actor_kinds": [...], "default_deny": [...],
//    "domains": [{name, owner, state_classes, types}...], "rejected": [{domain, claimant, holder,
//    code, message}...], "f2": {numerator, denominator, conflicts}}
//
// tools/fabric/ownership_check.py compares it with SL-ARCH-001 and the schema files. Every refused
// claim is also printed to stderr. Exit 0 when every claim was accepted, 1 when one was refused
// (the file is written either way: neg-fabric-f2 reads it), 2 on bad usage or a write failure.

#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/type.hpp"

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
        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
    }
  }
  out += '"';
  return out;
}

template <typename Range, typename F> std::string array(const Range &items, const F &each) {
  std::string out = "[";
  bool first = true;
  for (const auto &item : items) {
    out += first ? "" : ", ";
    out += each(item);
    first = false;
  }
  out += ']';
  return out;
}

std::string names(std::span<const std::string_view> items) {
  return array(items, [](std::string_view s) { return quote(s); });
}

std::string classes(std::span<const state_class> items) {
  return array(items, [](state_class s) { return quote(to_string(s)); });
}

std::string dump(const domain_registry &reg) {
  std::string out = "{\"format\": \"sl-fabric-domains/1\",\n";
  out += " \"reserved\": [\n";
  bool first = true;
  for (const reserved_prefix &row : reserved_prefixes()) {
    out += first ? "" : ",\n";
    out += std::format(R"(  {{"prefix": {}, "owner": {}, "owns": {}, "state_classes": {}}})",
                       quote(row.prefix), quote(owner_code(row.owner)), quote(row.owns),
                       classes(row.state_classes));
    first = false;
  }
  out += "],\n";
  out += std::format(" \"fabric_domains\": {},\n", names(fabric_domains()));
  out += std::format(" \"actor_kinds\": {},\n", names(actor_kind_names()));
  out += std::format(" \"default_deny\": {},\n", names(default_deny_names()));
  out += " \"domains\": [";
  first = true;
  for (const auto &[name, spec] : reg.domains()) {
    out += first ? "\n" : ",\n";
    out += std::format(R"(  {{"name": {}, "owner": {}, "state_classes": {}, "types": {}}})",
                       quote(name), quote(owner_code(spec.owner)), classes(spec.state_classes),
                       array(spec.types, [](const type_id &t) { return quote(t.to_string()); }));
    first = false;
  }
  out += "],\n \"rejected\": [";
  first = true;
  for (const rejected_claim &r : reg.rejected()) {
    out += first ? "\n" : ",\n";
    out += std::format(
        R"(  {{"domain": {}, "claimant": {}, "holder": {}, "code": {}, "message": {}}})",
        quote(r.domain), quote(owner_code(r.claimant)),
        r.holder ? quote(owner_code(*r.holder)) : std::string("null"), quote(sl::to_string(r.code)),
        quote(r.message));
    first = false;
  }
  const f2_report f2 = reg.f2();
  out +=
      std::format("],\n \"f2\": {{\"numerator\": {}, \"denominator\": {}, \"conflicts\": {}}}}}\n",
                  f2.numerator, f2.denominator, f2.conflicts);
  return out;
}

int run(std::span<char *> args) {
  std::vector<std::pair<std::string, std::string>> extra;
  std::optional<std::string> out_path;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--claim" && i + 1 < args.size()) {
      const std::string_view c = args[++i];
      const std::size_t eq = c.find('=');
      if (eq == std::string_view::npos) {
        std::println(stderr, "sl_fabric_dump_domains: --claim needs <domain>=<owner>, got {}", c);
        return 2;
      }
      extra.emplace_back(std::string(c.substr(0, eq)), std::string(c.substr(eq + 1)));
    } else if (!out_path) {
      out_path = std::string(a);
    } else {
      out_path.reset();
      break;
    }
  }
  if (!out_path) {
    std::println(stderr,
                 "usage: sl_fabric_dump_domains [--claim <domain>=<owner>]... <out.json|->");
    return 2;
  }
  domain_registry reg;
  if (auto ok = claim_fabric_domains(reg); !ok) {
    std::println(stderr, "sl_fabric_dump_domains: {}", sl::describe(ok.error()));
  }
  std::size_t refused = 0;
  for (const auto &[domain, owner] : extra) {
    const auto who = subsystem_from_string(owner);
    if (!who) {
      std::println(stderr, "sl_fabric_dump_domains: --claim {}: unknown owner {}", domain, owner);
      return 2;
    }
    const reserved_prefix *row = find_reserved_prefix(domain);
    domain_spec spec{.name = domain, .owner = *who, .state_classes = {}, .types = {}};
    if (row != nullptr && !row->state_classes.empty()) {
      spec.state_classes.push_back(row->state_classes.front());
    }
    if (!reg.claim(std::move(spec))) {
      ++refused; // recorded in rejected() with its reason
    }
  }
  const std::string text = dump(reg);
  if (*out_path == "-") {
    std::fputs(text.c_str(), stdout);
  } else {
    std::ofstream out(*out_path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
      std::println(stderr, "sl_fabric_dump_domains: cannot write {}", *out_path);
      return 2;
    }
  }
  for (const rejected_claim &r : reg.rejected()) {
    std::println(stderr, "sl_fabric_dump_domains: rejected claim of {} by {}: {}: {}", r.domain,
                 owner_code(r.claimant), sl::to_string(r.code), r.message);
  }
  return refused == 0 && reg.rejected().empty() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    return run(args);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_dump_domains: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_dump_domains: unknown error\n", stderr);
  }
  return 2;
}
