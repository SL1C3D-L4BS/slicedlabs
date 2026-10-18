// Validator registry, lock authority and receipt builder (WO-043, SL-ARCH-002 ARCH2-R05,
// ARCH2-R46; ADR-0019).

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

std::string_view to_string(severity s) noexcept {
  switch (s) {
  case severity::info:
    return "info";
  case severity::warning:
    return "warning";
  case severity::error:
    return "error";
  }
  return "unknown";
}

std::string_view to_string(validator_stage s) noexcept {
  switch (s) {
  case validator_stage::structural:
    return "structural";
  case validator_stage::schema:
    return "schema";
  case validator_stage::authority:
    return "authority";
  case validator_stage::references:
    return "references";
  case validator_stage::invariants:
    return "invariants";
  case validator_stage::policy:
    return "policy";
  case validator_stage::locks:
    return "locks";
  }
  return "unknown";
}

bool diagnostic_less(const diagnostic &a, const diagnostic &b) noexcept {
  if (a.stage != b.stage) {
    return a.stage < b.stage;
  }
  if (const auto c = a.record <=> b.record; std::is_neq(c)) {
    return std::is_lt(c);
  }
  if (const auto c = a.path <=> b.path; std::is_neq(c)) {
    return std::is_lt(c);
  }
  if (const auto c = a.rule <=> b.rule; std::is_neq(c)) {
    return std::is_lt(c);
  }
  return a.message < b.message;
}

bool has_error(std::span<const diagnostic> diagnostics) noexcept {
  return std::ranges::any_of(diagnostics,
                             [](const diagnostic &d) { return d.severity == severity::error; });
}

validation_context::validation_context(const fabric::patch &normalized, const staged_state &after,
                                       const schema_registry &schemas, edit_origin origin)
    : patch_(&normalized), after_(&after), schemas_(&schemas), origin_(std::move(origin)),
      footprint_(footprint_of(normalized)) {}

result<void> validator_registry::add(std::shared_ptr<const validator> v) {
  if (frozen_) {
    return make_error(errc::registry_frozen, "the validator registry is frozen");
  }
  if (!v) {
    return make_error(errc::invalid_argument, "a null validator");
  }
  const std::string_view name = v->name();
  if (std::ranges::any_of(validators_, [&](const auto &x) { return x->name() == name; })) {
    return make_error(errc::already_exists,
                      std::format("validator {} is registered already", name));
  }
  validators_.push_back(std::move(v));
  return {};
}

std::vector<const validator *> validator_registry::in_order() const {
  std::vector<const validator *> out;
  out.reserve(validators_.size());
  for (const auto &v : validators_) {
    out.push_back(v.get());
  }
  std::ranges::stable_sort(out, {}, [](const validator *v) { return v->stage(); });
  return out;
}

result<std::vector<diagnostic>> validator_registry::run(const validation_context &ctx) const {
  std::vector<diagnostic> all;
  for (const validator *v : in_order()) {
    std::vector<diagnostic> found;
    if (auto ok = v->run(ctx, found); !ok) {
      return make_error(ok.error().code,
                        std::format("validator {}: {}", v->name(), ok.error().message));
    }
    for (diagnostic &d : found) {
      d.stage = v->stage();
      all.push_back(std::move(d));
    }
  }
  std::ranges::stable_sort(all, diagnostic_less);
  return all;
}

result<void> null_lock_authority::check(const edit_origin & /*origin*/,
                                        const footprint & /*fp*/) const {
  return {};
}

result<void> receipt_builder::attach(std::string name, value summary) {
  if (attachments_.contains(name)) {
    return make_error(errc::already_exists, std::format("receipt attachment {} exists", name));
  }
  attachments_.emplace(std::move(name), std::move(summary));
  return {};
}

const value *receipt_builder::find(std::string_view name) const noexcept {
  const auto it = attachments_.find(name);
  return it == attachments_.end() ? nullptr : &it->second;
}

} // namespace sl::fabric
