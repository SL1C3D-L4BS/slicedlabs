// Epistemic classes, the F3 provenance record and the derivation key (WO-049, SL-ARCH-001
// ARCH1-R42 to ARCH1-R44).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.provenance.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

bool is_zero(const digest &d) noexcept {
  return std::ranges::all_of(d.bytes, [](std::uint8_t b) { return b == 0; });
}

struct input_pair {
  record_id id;
  digest rev;
};

} // namespace

std::string_view to_string(epistemic_class c) noexcept {
  switch (c) {
  case epistemic_class::observed:
    return "observed";
  case epistemic_class::imported:
    return "imported";
  case epistemic_class::asserted:
    return "asserted";
  case epistemic_class::calculated:
    return "calculated";
  case epistemic_class::inferred:
    return "inferred";
  case epistemic_class::accepted:
    return "accepted";
  }
  return "unknown";
}

result<void> check_transition(epistemic_class from, epistemic_class to, const actor &who,
                              std::string_view domain) {
  if (!is_legal_transition(from, to)) {
    const char *why = "only imported, asserted, calculated and inferred records promote, and "
                      "only to accepted";
    if (from == epistemic_class::observed) {
      why = "observed records are immutable; a new observation supersedes";
    } else if (from == epistemic_class::accepted) {
      why = "accepted records are final; demotion is a new asserted record that supersedes";
    } else if (from == to) {
      why = "a write that keeps its class is not a transition";
    }
    return make_error(errc::invariant_violated,
                      std::format("epistemic transition {} -> {} is not allowed: {}",
                                  to_string(from), to_string(to), why));
  }
  if (!holds_grant(who, "accept", domain)) {
    return make_error(errc::promotion_denied,
                      std::format("actor {} ({}) may not promote {} -> {} in domain '{}'; it "
                                  "needs the grant accept:{}",
                                  who.id.to_string(), to_string(who.kind), to_string(from),
                                  to_string(to), domain, domain));
  }
  return {};
}

std::optional<std::string_view> first_missing_field(const provenance &p) noexcept {
  const bool ids_ok =
      p.input_ids && !p.input_ids->empty() &&
      std::ranges::none_of(*p.input_ids, [](const record_id &id) { return id.is_null(); });
  if (!ids_ok) {
    return f3_field_names[0];
  }
  const bool revs_ok = p.input_revisions && p.input_revisions->size() == p.input_ids->size() &&
                       std::ranges::none_of(*p.input_revisions, is_zero);
  if (!revs_ok) {
    return f3_field_names[1];
  }
  if (!p.toolchain_id || p.toolchain_id->empty()) {
    return f3_field_names[2];
  }
  if (!p.parameters_digest || is_zero(*p.parameters_digest)) {
    return f3_field_names[3];
  }
  if (!p.environment_hash || is_zero(*p.environment_hash)) {
    return f3_field_names[4];
  }
  return std::nullopt;
}

result<void> check_provenance(const provenance &p) {
  if (const auto missing = first_missing_field(p); missing) {
    std::string detail = "is missing";
    if (*missing == f3_field_names[0]) {
      detail = "is missing, empty or holds a null id";
    } else if (*missing == f3_field_names[1]) {
      detail = "is missing, has a different length than input_ids or holds a zero digest";
    } else if (*missing == f3_field_names[2]) {
      detail = "is missing or empty";
    } else {
      detail = "is missing or the zero digest";
    }
    return make_error(errc::provenance_incomplete,
                      std::format("provenance field '{}' {}", *missing, detail));
  }
  return {};
}

result<std::vector<std::uint8_t>> derivation_preimage(const provenance &p) {
  if (auto ok = check_provenance(p); !ok) {
    return std::unexpected(ok.error());
  }
  if (!p.input_ids || !p.input_revisions || !p.toolchain_id || !p.parameters_digest ||
      !p.environment_hash) {
    return make_error(errc::provenance_incomplete, "provenance is incomplete");
  }
  std::vector<input_pair> pairs;
  pairs.reserve(p.input_ids->size());
  for (std::size_t i = 0; i < p.input_ids->size(); ++i) {
    pairs.push_back(input_pair{.id = (*p.input_ids)[i], .rev = (*p.input_revisions)[i]});
  }
  std::ranges::sort(pairs, [](const input_pair &a, const input_pair &b) {
    return std::tie(a.id, a.rev) < std::tie(b.id, b.rev);
  });
  std::vector<value> inputs;
  inputs.reserve(pairs.size());
  for (const input_pair &pr : pairs) {
    auto v = gen::fabric_provenance::input_ref_to_value(
        gen::fabric_provenance::input_ref{.id = pr.id, .digest = pr.rev});
    if (!v) {
      return std::unexpected(v.error());
    }
    inputs.push_back(std::move(*v));
  }
  auto list = value::list(std::move(inputs));
  if (!list) {
    return std::unexpected(list.error());
  }
  auto tool = value::string(*p.toolchain_id);
  if (!tool) {
    return std::unexpected(tool.error());
  }
  auto body =
      value::struct_({field{.no = 1, .v = std::move(*list)}, field{.no = 2, .v = std::move(*tool)},
                      field{.no = 3, .v = value::digest(*p.parameters_digest)},
                      field{.no = 4, .v = value::digest(*p.environment_hash)}});
  if (!body) {
    return std::unexpected(body.error());
  }
  return encode(*body);
}

result<digest> derivation_key(const provenance &p) {
  auto bytes = derivation_preimage(p);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return digest{.algo = hash_algo::sha256, .bytes = sha256::hash(*bytes)};
}

} // namespace sl::fabric
