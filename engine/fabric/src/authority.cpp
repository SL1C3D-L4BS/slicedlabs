// Actors, grants and the authority validator (WO-048, SL-ARCH-001 ARCH1-R39, ARCH1-R40,
// ADR-0017).

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::array<std::pair<actor_kind, std::string_view>, 5> kind_names{{
    {actor_kind::human, "human"},
    {actor_kind::tool, "tool"},
    {actor_kind::plugin, "plugin"},
    {actor_kind::agent, "agent"},
    {actor_kind::service, "service"},
}};

std::optional<std::string_view> grant_target(std::string_view grant, std::string_view verb) {
  if (!grant.starts_with(verb) || grant.size() <= verb.size() || grant[verb.size()] != ':') {
    return std::nullopt;
  }
  return grant.substr(verb.size() + 1);
}

bool is_grant_target(std::string_view target) noexcept {
  if (find_reserved_prefix(target) == nullptr) {
    return false;
  }
  return !target.contains('.') || is_domain_name(target);
}

std::string describe(const actor &a) {
  return std::format("actor {} ({})", a.id.to_string(), to_string(a.kind));
}

// One domain an op needs, at the first path that needed it.
struct requirement {
  record_id record;
  path at;
  std::string domain;
};

// Walks the typed values an op touches and collects the domain boundaries (file comment of
// authority.hpp).
class boundary_walker {
public:
  boundary_walker(const schema_registry &schemas, const domain_registry &domains,
                  std::vector<requirement> &out) noexcept
      : schemas_(&schemas), domains_(&domains), out_(&out) {}

  void set_record(const record_id &record) noexcept { record_ = record; }

  // The record type's domain: always a requirement, at the root path. nullopt for an unknown
  // type.
  std::optional<std::string> enter_record(const type_id &type) {
    const type_desc *t = schemas_->find(type);
    if (t == nullptr) {
      return std::nullopt;
    }
    std::string domain = own_domain(*t).value_or(std::string{});
    need(path{}, domain);
    return domain;
  }

  // Every boundary inside `v`, typed `type`, whose parent domain is `parent`, at `at`.
  void walk_value(const value &v, const type_id &type, const std::string &parent, path &at) {
    const type_desc *t = schemas_->find(type);
    if (t == nullptr) {
      return;
    }
    const std::string domain = enter(*t, parent, at);
    switch (t->kind) {
    case type_kind::tagged:
      if (t->element) {
        walk_value(v, *t->element, domain, at);
      }
      break;
    case type_kind::struct_:
      if (v.kind() == value_kind::struct_) {
        for (const field &f : v.fields()) {
          if (const field_desc *fd = t->find_field(f.no); fd != nullptr) {
            at.push_back(path_segment::field(f.no));
            walk_value(f.v, fd->type, domain, at);
            at.pop_back();
          }
        }
      }
      break;
    case type_kind::variant:
      if (v.kind() == value_kind::variant) {
        if (const case_desc *cd = t->find_case(v.case_no()); cd != nullptr) {
          at.push_back(path_segment::field(v.case_no()));
          walk_value(v.payload(), cd->type, domain, at);
          at.pop_back();
        }
      }
      break;
    case type_kind::list:
      if (v.kind() == value_kind::list && t->element) {
        for (const value &item : v.items()) {
          walk_value(item, *t->element, domain, at); // lists are atomic: the list's path
        }
      }
      break;
    case type_kind::map:
      if (v.kind() == value_kind::map && t->element) {
        for (const map_entry &e : v.entries()) {
          if (auto seg = path_segment::key(e.key); seg) {
            at.push_back(std::move(*seg));
            walk_value(e.v, *t->element, domain, at);
            at.pop_back();
          }
        }
      }
      break;
    case type_kind::oset:
      if (v.kind() == value_kind::oset && t->element) {
        for (const oset_entry &e : v.elements()) {
          at.push_back(path_segment::element(e.element));
          walk_value(e.v, *t->element, domain, at);
          at.pop_back();
        }
      }
      break;
    default:
      break;
    }
  }

  // Follows `p` from the record type, collecting the boundaries along it. The type and domain
  // at the end of the path; nullopt when the path leaves the schema (left to other stages).
  std::optional<std::pair<type_id, std::string>>
  walk_path(const type_id &root, const std::string &root_domain, const path &p) {
    type_id cur = root;
    std::string domain = root_domain;
    path at;
    for (const path_segment &seg : p) {
      const type_desc *t = resolve_tagged(cur, domain, at);
      if (t == nullptr) {
        return std::nullopt;
      }
      std::optional<type_id> next;
      if (seg.kind() == path_segment::kind_type::field && t->kind == type_kind::struct_) {
        if (const field_desc *fd = t->find_field(seg.field_no()); fd != nullptr) {
          next = fd->type;
        }
      } else if (seg.kind() == path_segment::kind_type::field && t->kind == type_kind::variant) {
        if (const case_desc *cd = t->find_case(seg.field_no()); cd != nullptr) {
          next = cd->type;
        }
      } else if ((seg.kind() == path_segment::kind_type::key && t->kind == type_kind::map) ||
                 (seg.kind() == path_segment::kind_type::element && t->kind == type_kind::oset)) {
        next = t->element;
      }
      if (!next) {
        return std::nullopt;
      }
      at.push_back(seg);
      const type_desc *n = schemas_->find(*next);
      if (n == nullptr) {
        return std::nullopt;
      }
      domain = enter(*n, domain, at);
      cur = *next;
    }
    return std::pair{cur, domain};
  }

private:
  // The domain a type declares: its own, else the domain its id is bound to; nullopt to
  // inherit (built-in types, empty domains).
  [[nodiscard]] std::optional<std::string> own_domain(const type_desc &t) const {
    if (is_builtin_type(t.id)) {
      return std::nullopt;
    }
    if (!t.domain.empty()) {
      return t.domain;
    }
    if (const domain_spec *spec = domains_->domain_of(t.id); spec != nullptr) {
      return spec->name;
    }
    return std::nullopt;
  }

  // Enters a typed value: a boundary when its domain differs from the parent's.
  std::string enter(const type_desc &t, const std::string &parent, const path &at) {
    std::optional<std::string> own = own_domain(t);
    if (!own || *own == parent) {
      return parent;
    }
    need(at, *own);
    return *own;
  }

  const type_desc *resolve_tagged(type_id &cur, std::string &domain, const path &at) {
    const type_desc *t = schemas_->find(cur);
    for (std::size_t guard = 0; t != nullptr && t->kind == type_kind::tagged; ++guard) {
      if (!t->element || guard > max_value_depth) {
        return nullptr;
      }
      cur = *t->element;
      t = schemas_->find(cur);
      if (t != nullptr) {
        domain = enter(*t, domain, at);
      }
    }
    return t;
  }

  void need(const path &at, const std::string &domain) {
    if (domain.empty()) {
      return;
    }
    const bool seen = std::ranges::any_of(
        *out_, [&](const requirement &r) { return r.record == record_ && r.domain == domain; });
    if (!seen) {
      out_->push_back(requirement{.record = record_, .at = at, .domain = domain});
    }
  }

  const schema_registry *schemas_;
  const domain_registry *domains_;
  std::vector<requirement> *out_;
  record_id record_;
};

path with_element(const path &p, const id_bytes &elem) {
  path out = p;
  out.push_back(path_segment::element(elem));
  return out;
}

// The requirements of one op. `type` is the record's type as the op sees it (null when the
// record is absent before and after).
void collect(boundary_walker &w, const patch_op &op, const type_id &type) {
  w.set_record(op.target);
  switch (op.kind) {
  case op_kind::create_record:
    if (const auto d = w.enter_record(op.type); d && op.new_value) {
      path at;
      w.walk_value(*op.new_value, op.type, *d, at);
    }
    break;
  case op_kind::delete_record: {
    const type_id t = op.type.is_null() ? type : op.type;
    if (const auto d = w.enter_record(t); d && op.captured) {
      path at;
      w.walk_value(*op.captured, t, *d, at);
    }
    break;
  }
  case op_kind::retype_record:
    if (const auto d = w.enter_record(op.type); d && op.new_value) {
      path at;
      w.walk_value(*op.new_value, op.type, *d, at);
    }
    if (const auto d = w.enter_record(op.expect_type); d && op.captured) {
      path at;
      w.walk_value(*op.captured, op.expect_type, *d, at);
    }
    break;
  case op_kind::set:
  case op_kind::unset:
  case op_kind::insert_elem:
  case op_kind::remove_elem:
  case op_kind::move_elem: {
    const auto d = w.enter_record(type);
    if (!d) {
      break;
    }
    const bool element_op = op.kind == op_kind::insert_elem || op.kind == op_kind::remove_elem ||
                            op.kind == op_kind::move_elem;
    path full = element_op ? with_element(op.path, op.elem) : op.path;
    const auto end = w.walk_path(type, *d, full);
    if (!end) {
      break;
    }
    if (op.new_value) {
      w.walk_value(*op.new_value, end->first, end->second, full);
    }
    if (op.expect.is_present()) {
      w.walk_value(op.expect.expected(), end->first, end->second, full);
    }
    if (op.captured) {
      w.walk_value(*op.captured, end->first, end->second, full);
    }
    break;
  }
  case op_kind::annotate:
    break;
  }
}

} // namespace

std::string_view to_string(actor_kind k) noexcept {
  for (const auto &[kind, name] : kind_names) {
    if (kind == k) {
      return name;
    }
  }
  return "unknown";
}

std::optional<actor_kind> actor_kind_from_string(std::string_view name) noexcept {
  for (const auto &[kind, n] : kind_names) {
    if (n == name) {
      return kind;
    }
  }
  return std::nullopt;
}

bool denied_by_default(actor_kind k) noexcept {
  const std::string_view name = to_string(k);
  return std::ranges::find(default_deny_names(), name) != default_deny_names().end();
}

bool is_valid_grant(std::string_view grant) noexcept {
  for (const std::string_view verb : {std::string_view{"write"}, std::string_view{"accept"}}) {
    if (const auto target = grant_target(grant, verb); target) {
      return is_grant_target(*target);
    }
  }
  return false;
}

bool holds_grant(const actor &a, std::string_view verb, std::string_view domain) noexcept {
  return std::ranges::any_of(a.grants, [&](const std::string &g) {
    const auto target = grant_target(g, verb);
    return target && domain_within(domain, *target);
  });
}

result<void> check_write(const domain_registry &domains, const actor &a, std::string_view subsystem,
                         std::string_view domain) {
  const domain_spec *owner = domains.owner_of(domain);
  if (owner == nullptr) {
    return make_error(
        errc::ownership_orphan,
        std::format("domain '{}' has no claimed owner; {} may not write it", domain, describe(a)));
  }
  if (holds_grant(a, "write", domain)) {
    return {};
  }
  if (denied_by_default(a.kind)) {
    return make_error(errc::authority_denied,
                      std::format("{} is denied by default: writing {} needs the grant write:{}",
                                  describe(a), domain, domain));
  }
  if (subsystem_from_string(subsystem) == owner->owner) {
    return {};
  }
  return make_error(errc::authority_denied,
                    std::format("{} via subsystem '{}' may not write {} (owner {} {}); it needs "
                                "the grant write:{}",
                                describe(a), subsystem, domain, owner_code(owner->owner),
                                to_string(owner->owner), domain));
}

result<void> check_accept(const domain_registry &domains, const actor &a, std::string_view domain) {
  if (domains.owner_of(domain) == nullptr) {
    return make_error(
        errc::ownership_orphan,
        std::format("domain '{}' has no claimed owner; {} may not accept it", domain, describe(a)));
  }
  if (holds_grant(a, "accept", domain)) {
    return {};
  }
  return make_error(errc::authority_denied,
                    std::format("{} may not accept records of {}; it needs the grant accept:{}",
                                describe(a), domain, domain));
}

result<void> actor_directory::add(actor a) {
  if (a.id.is_null()) {
    return make_error(errc::invalid_argument, "an actor needs a non-null id");
  }
  if (to_string(a.kind) == "unknown") {
    return make_error(errc::invalid_argument,
                      std::format("actor {} has an unknown kind", a.id.to_string()));
  }
  for (const std::string &g : a.grants) {
    if (!is_valid_grant(g)) {
      return make_error(errc::invalid_argument,
                        std::format("actor {}: malformed grant '{}'", a.id.to_string(), g));
    }
  }
  if (actors_.contains(a.id)) {
    return make_error(errc::already_exists,
                      std::format("actor {} is in the directory already", a.id.to_string()));
  }
  const actor_id id = a.id;
  actors_.emplace(id, std::move(a));
  return {};
}

const actor *actor_directory::find(const actor_id &id) const noexcept {
  const auto it = actors_.find(id);
  return it == actors_.end() ? nullptr : &it->second;
}

result<void> authority_validator::run(const validation_context &ctx,
                                      std::vector<diagnostic> &out) const {
  const patch &p = ctx.patch();
  const bool mutates =
      std::ranges::any_of(p.ops, [](const patch_op &op) { return op.kind != op_kind::annotate; });
  if (!mutates) {
    return {};
  }
  const actor *who = actors_->find(ctx.origin().actor);
  if (who == nullptr) {
    out.push_back(diagnostic{.rule = "R-AUTH-ACTOR",
                             .severity = severity::error,
                             .record = record_id{},
                             .path = {},
                             .message = std::format("actor {} is not in the actor directory; "
                                                    "unknown actors are denied",
                                                    ctx.origin().actor.to_string()),
                             .stage = validator_stage::authority});
    return {};
  }
  std::vector<requirement> needs;
  boundary_walker walker(ctx.schemas(), *domains_, needs);
  for (const patch_op &op : p.ops) {
    if (op.kind == op_kind::annotate) {
      continue;
    }
    type_id type;
    if (op.kind != op_kind::create_record) {
      auto after = ctx.after().read(op.target);
      if (!after) {
        return make_error(after.error().code, after.error().message);
      }
      if (*after) {
        type = (*after)->type;
      } else {
        auto before = ctx.before().read(op.target);
        if (!before) {
          return make_error(before.error().code, before.error().message);
        }
        if (*before) {
          type = (*before)->type;
        }
      }
    }
    collect(walker, op, type);
  }
  for (const requirement &r : needs) {
    if (auto ok = check_write(*domains_, *who, ctx.origin().subsystem, r.domain); !ok) {
      const bool orphan = ok.error().code == errc::ownership_orphan;
      out.push_back(diagnostic{.rule = orphan ? "R-AUTH-ORPHAN" : "R-AUTH-DENIED",
                               .severity = severity::error,
                               .record = r.record,
                               .path = r.at,
                               .message = ok.error().message,
                               .stage = validator_stage::authority});
    }
  }
  return {};
}

} // namespace sl::fabric
