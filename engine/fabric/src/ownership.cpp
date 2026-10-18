// The domain registry, the F2 enforcement point (WO-048, SL-ARCH-001 ARCH1-R36 to ARCH1-R38,
// ADR-0017).

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

namespace {

constexpr std::array<state_class, 1> sc_document{state_class::document};
constexpr std::array<state_class, 2> sc_document_derived{state_class::document,
                                                         state_class::derived};
constexpr std::array<state_class, 1> sc_observation{state_class::observation};
constexpr std::array<state_class, 1> sc_derived{state_class::derived};
constexpr std::array<state_class, 2> sc_observation_derived{state_class::observation,
                                                            state_class::derived};
constexpr std::array<state_class, 2> sc_observation_document{state_class::observation,
                                                             state_class::document};
constexpr std::array<state_class, 2> sc_document_observation{state_class::document,
                                                             state_class::observation};

// SL-ARCH-001 `domain_registry`, row for row (tools/fabric/ownership_check.py compares the dump of
// this table with the document).
constexpr std::array<reserved_prefix, subsystem_count> reserved_table{{
    {.prefix = "shell",
     .owner = subsystem::shell,
     .owns = "workspace layouts, command registry, selection",
     .state_classes = sc_document},
    {.prefix = "fabric",
     .owner = subsystem::fabric,
     .owns =
         "ids, schemas, revisions, transactions, provenance, manifest, tombstones, import records",
     .state_classes = sc_document_derived},
    {.prefix = "runtime",
     .owner = subsystem::runtime,
     .owns = "captured runtime observations only; live state is transient and never a record",
     .state_classes = sc_observation},
    {.prefix = "gfx",
     .owner = subsystem::gfx,
     .owns = "GPU resource and pipeline records (derived)",
     .state_classes = sc_derived},
    {.prefix = "content",
     .owner = subsystem::content,
     .owns = "editable source documents, including game UI (content.ui)",
     .state_classes = sc_document},
    {.prefix = "sim",
     .owner = subsystem::sim,
     .owns = "gameplay config, nav settings, net topology",
     .state_classes = sc_document_derived},
    {.prefix = "code",
     .owner = subsystem::code,
     .owns = "source files, graph IR",
     .state_classes = sc_document_derived},
    {.prefix = "asset",
     .owner = subsystem::asset,
     .owns = "build DAG, cooked artifact records; bytes live in the Fabric object store (ADR-0014)",
     .state_classes = sc_document_derived},
    {.prefix = "obs",
     .owner = subsystem::obs,
     .owns = "traces, captures, checkpoints",
     .state_classes = sc_observation_derived},
    {.prefix = "collab",
     .owner = subsystem::collab,
     .owns = "accepted revisions, branch policy, locks and leases",
     .state_classes = sc_document},
    {.prefix = "verify",
     .owner = subsystem::verify,
     .owns = "test results, release manifests",
     .state_classes = sc_observation_document},
    {.prefix = "gov",
     .owner = subsystem::gov,
     .owns = "agent permissions, approvals, audit log",
     .state_classes = sc_document_observation},
}};

constexpr std::array<std::string_view, 6> fabric_domain_table{
    "fabric.history",  "fabric.schema",     "fabric.manifest",
    "fabric.identity", "fabric.provenance", "fabric.edge"};

constexpr std::array<std::string_view, 5> actor_kind_table{"human", "tool", "plugin", "agent",
                                                           "service"};
constexpr std::array<std::string_view, 2> default_deny_table{"agent", "service"};

constexpr std::array<std::string_view, subsystem_count> owner_codes{
    "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12"};

bool is_identifier(std::string_view s) noexcept {
  if (s.empty() || s.front() < 'a' || s.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(
      s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::string_view first_segment(std::string_view domain) noexcept {
  return domain.substr(0, domain.find('.'));
}

std::string describe_owner(subsystem s) {
  return std::format("{} ({})", owner_code(s), to_string(s));
}

bool admitted(const reserved_prefix &row, state_class sc) noexcept {
  return std::ranges::find(row.state_classes, sc) != row.state_classes.end();
}

} // namespace

std::string_view to_string(subsystem s) noexcept {
  for (const reserved_prefix &row : reserved_table) {
    if (row.owner == s) {
      return row.prefix;
    }
  }
  return "unknown";
}

std::string_view owner_code(subsystem s) noexcept {
  const auto n = static_cast<std::size_t>(s);
  if (n == 0 || n > subsystem_count) {
    return "00";
  }
  return *std::next(owner_codes.begin(), static_cast<std::ptrdiff_t>(n - 1));
}

std::optional<subsystem> subsystem_from_string(std::string_view text) noexcept {
  for (const reserved_prefix &row : reserved_table) {
    if (row.prefix == text || owner_code(row.owner) == text) {
      return row.owner;
    }
  }
  return std::nullopt;
}

std::span<const reserved_prefix> reserved_prefixes() noexcept { return reserved_table; }

const reserved_prefix *find_reserved_prefix(std::string_view domain) noexcept {
  const std::string_view head = first_segment(domain);
  const auto *it = std::ranges::find(reserved_table, head, &reserved_prefix::prefix);
  return it == reserved_table.end() ? nullptr : &*it;
}

std::span<const std::string_view> fabric_domains() noexcept { return fabric_domain_table; }
std::span<const std::string_view> actor_kind_names() noexcept { return actor_kind_table; }
std::span<const std::string_view> default_deny_names() noexcept { return default_deny_table; }

bool is_domain_name(std::string_view name) noexcept {
  std::size_t segments = 0;
  while (true) {
    const std::size_t dot = name.find('.');
    if (!is_identifier(name.substr(0, dot))) {
      return false;
    }
    ++segments;
    if (dot == std::string_view::npos) {
      return segments >= 2;
    }
    name.remove_prefix(dot + 1);
  }
}

bool domain_within(std::string_view domain, std::string_view ancestor) noexcept {
  if (ancestor.empty() || !domain.starts_with(ancestor)) {
    return false;
  }
  return domain.size() == ancestor.size() || domain[ancestor.size()] == '.';
}

result<void> domain_registry::check_claim(const domain_spec &spec) const {
  if (frozen_) {
    return make_error(errc::registry_frozen,
                      std::format("claim of {} by {}: the domain registry is frozen", spec.name,
                                  describe_owner(spec.owner)));
  }
  if (owner_code(spec.owner) == "00") {
    return make_error(errc::invalid_argument,
                      std::format("claim of {}: owner is not a subsystem 01..12", spec.name));
  }
  const reserved_prefix *row =
      is_domain_name(spec.name) ? find_reserved_prefix(spec.name) : nullptr;
  if (row == nullptr) {
    return make_error(errc::ownership_orphan,
                      std::format("domain '{}' claimed by {} is not a dotted name under a reserved "
                                  "prefix",
                                  spec.name, describe_owner(spec.owner)));
  }
  if (const auto it = domains_.find(spec.name);
      it != domains_.end() && it->second.owner != spec.owner) {
    return make_error(errc::ownership_conflict,
                      std::format("domain {} is owned by {}; a second claim by {} is refused",
                                  spec.name, describe_owner(it->second.owner),
                                  describe_owner(spec.owner)));
  }
  if (row->owner != spec.owner) {
    return make_error(errc::ownership_conflict,
                      std::format("domain {} is under prefix {} reserved to {}; a claim by {} is "
                                  "refused",
                                  spec.name, row->prefix, describe_owner(row->owner),
                                  describe_owner(spec.owner)));
  }
  if (row->owner == subsystem::fabric &&
      std::ranges::none_of(fabric_domain_table,
                           [&](std::string_view d) { return domain_within(spec.name, d); })) {
    return make_error(errc::ownership_orphan,
                      std::format("domain {} is not one of the fabric domains", spec.name));
  }
  if (spec.state_classes.empty()) {
    return make_error(errc::schema_invalid,
                      std::format("R-OWN-STATE: domain {} declares no state class", spec.name));
  }
  for (const state_class sc : spec.state_classes) {
    if (!admitted(*row, sc)) {
      return make_error(errc::schema_invalid,
                        std::format("R-OWN-STATE: state class {} is not admitted under prefix {}",
                                    to_string(sc), row->prefix));
    }
  }
  if (domains_.contains(spec.name)) {
    return make_error(errc::already_exists, std::format("domain {} is claimed by {} already",
                                                        spec.name, describe_owner(spec.owner)));
  }
  std::vector<type_id> seen;
  for (const type_id &t : spec.types) {
    if (t.is_null()) {
      return make_error(errc::invalid_argument,
                        std::format("domain {} lists a null type id", spec.name));
    }
    if (const auto it = type_domain_.find(t); it != type_domain_.end()) {
      return make_error(errc::ownership_conflict,
                        std::format("type {} is bound to domain {}; domain {} may not claim it",
                                    t.to_string(), it->second, spec.name));
    }
    if (std::ranges::find(seen, t) != seen.end()) {
      return make_error(errc::invalid_argument,
                        std::format("domain {} lists type {} twice", spec.name, t.to_string()));
    }
    seen.push_back(t);
  }
  return {};
}

result<void> domain_registry::reject(const domain_spec &spec, std::optional<subsystem> holder,
                                     const error &why) {
  rejected_.push_back(rejected_claim{.domain = spec.name,
                                     .claimant = spec.owner,
                                     .holder = holder,
                                     .code = why.code,
                                     .message = why.message});
  return make_error(why.code, why.message);
}

result<void> domain_registry::claim(domain_spec spec) {
  if (auto ok = check_claim(spec); !ok) {
    std::optional<subsystem> holder;
    if (ok.error().code == errc::ownership_conflict) {
      if (const auto it = domains_.find(spec.name); it != domains_.end()) {
        holder = it->second.owner;
      } else if (const reserved_prefix *row = find_reserved_prefix(spec.name);
                 row != nullptr && row->owner != spec.owner) {
        holder = row->owner;
      }
    }
    return reject(spec, holder, ok.error());
  }
  std::ranges::sort(spec.state_classes);
  for (const type_id &t : spec.types) {
    type_domain_.emplace(t, spec.name);
  }
  std::string name = spec.name;
  domains_.emplace(std::move(name), std::move(spec));
  return {};
}

const domain_spec *domain_registry::find(std::string_view domain) const noexcept {
  const auto it = domains_.find(domain);
  return it == domains_.end() ? nullptr : &it->second;
}

const domain_spec *domain_registry::owner_of(std::string_view domain) const noexcept {
  while (!domain.empty()) {
    if (const domain_spec *spec = find(domain); spec != nullptr) {
      return spec;
    }
    const std::size_t dot = domain.rfind('.');
    if (dot == std::string_view::npos) {
      break;
    }
    domain = domain.substr(0, dot);
  }
  return nullptr;
}

const domain_spec *domain_registry::domain_of(const type_id &type) const noexcept {
  const auto it = type_domain_.find(type);
  return it == type_domain_.end() ? nullptr : find(it->second);
}

result<void> domain_registry::bind_types(const schema_registry &schemas) {
  if (frozen_) {
    return make_error(errc::registry_frozen, "bind_types: the domain registry is frozen");
  }
  std::vector<std::string> orphans;
  std::vector<std::pair<type_id, std::string>> binds;
  for (const auto &[id, t] : schemas.types()) {
    if (is_builtin_type(id)) {
      continue;
    }
    const domain_spec *spec = t.domain.empty() ? nullptr : owner_of(t.domain);
    if (spec == nullptr) {
      orphans.push_back(t.domain.empty()
                            ? std::format("type '{}' has no domain", t.name)
                            : std::format("type '{}' has domain '{}', which no subsystem claims",
                                          t.name, t.domain));
      continue;
    }
    if (const auto it = type_domain_.find(id);
        it != type_domain_.end() && it->second != spec->name) {
      return make_error(errc::ownership_conflict,
                        std::format("type '{}' is bound to domain {}; its schema names {}", t.name,
                                    it->second, spec->name));
    }
    binds.emplace_back(id, spec->name);
  }
  if (!orphans.empty()) {
    std::ranges::sort(orphans);
    std::string message;
    for (const std::string &o : orphans) {
      message += message.empty() ? o : "; " + o;
    }
    return make_error(errc::ownership_orphan, std::move(message));
  }
  for (auto &[id, name] : binds) {
    type_domain_.insert_or_assign(id, std::move(name));
  }
  return {};
}

result<void> domain_registry::freeze() {
  if (frozen_) {
    return {};
  }
  if (const f2_report r = f2(); r.numerator != 0) {
    return make_error(
        errc::invariant_violated,
        std::format("F2: {} of {} domains have more than one owner", r.numerator, r.denominator));
  }
  frozen_ = true;
  return {};
}

f2_report domain_registry::f2() const {
  f2_report r;
  r.denominator = domains_.size();
  for (const auto &[name, spec] : domains_) {
    const reserved_prefix *row = find_reserved_prefix(name);
    if (row == nullptr || row->owner != spec.owner) {
      ++r.numerator;
    }
  }
  r.conflicts = static_cast<std::size_t>(
      std::ranges::count(rejected_, errc::ownership_conflict, &rejected_claim::code));
  return r;
}

result<void> claim_fabric_domains(domain_registry &registry) {
  const reserved_prefix *row = find_reserved_prefix("fabric");
  for (const std::string_view d : fabric_domain_table) {
    domain_spec spec{.name = std::string(d),
                     .owner = subsystem::fabric,
                     .state_classes = {row->state_classes.begin(), row->state_classes.end()},
                     .types = {}};
    if (auto ok = registry.claim(std::move(spec)); !ok) {
      return ok;
    }
  }
  return {};
}

} // namespace sl::fabric
