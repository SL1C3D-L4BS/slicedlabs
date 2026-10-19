// The provenance validator and its fixed source (WO-049, SL-ARCH-002 ARCH2-R14, SL-ARCH-001
// ARCH1-R42, ARCH1-R43, ARCH1-R45).

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

namespace {

diagnostic finding(std::string rule, const record_id &record, path at, errc code,
                   const std::string &message) {
  return diagnostic{.rule = std::move(rule),
                    .severity = severity::error,
                    .record = record,
                    .path = std::move(at),
                    .message = std::format("{}: {}", to_string(code), message),
                    .stage = validator_stage::invariants};
}

// The type of `id` after the patch, else before it; null when the record does not exist.
result<type_id> type_of(const validation_context &ctx, const record_id &id) {
  auto after = ctx.after().read(id);
  if (!after) {
    return std::unexpected(after.error());
  }
  if (*after) {
    return (*after)->type;
  }
  auto before = ctx.before().read(id);
  if (!before) {
    return std::unexpected(before.error());
  }
  return *before ? (*before)->type : type_id{};
}

} // namespace

result<std::optional<epistemic_class>>
provenance_source::origin_class([[maybe_unused]] const validation_context &ctx) const {
  return std::optional<epistemic_class>{};
}

result<std::optional<provenance>>
fixed_provenance_source::origin([[maybe_unused]] const validation_context &ctx) const {
  return origin_;
}

result<std::optional<provenance>>
fixed_provenance_source::override_for([[maybe_unused]] const validation_context &ctx,
                                      std::size_t op_index) const {
  const auto it = overrides_.find(op_index);
  if (it == overrides_.end()) {
    return std::optional<provenance>{};
  }
  return std::optional<provenance>{it->second};
}

result<std::vector<class_change>>
fixed_provenance_source::class_changes([[maybe_unused]] const validation_context &ctx) const {
  return changes_;
}

result<void> provenance_validator::run(const validation_context &ctx,
                                       std::vector<diagnostic> &out) const {
  const patch &p = ctx.patch();

  // F3: a derived record needs complete provenance on the origin or the op.
  std::optional<std::optional<provenance>> origin; // read once, on the first derived write
  for (std::size_t i = 0; i < p.ops.size(); ++i) {
    const patch_op &op = p.ops[i];
    if (op.kind == op_kind::annotate || op.kind == op_kind::delete_record) {
      continue;
    }
    auto after = ctx.after().read(op.target);
    if (!after) {
      return std::unexpected(after.error());
    }
    if (!*after) {
      continue;
    }
    const type_desc *t = ctx.schemas().find((*after)->type);
    if (t == nullptr || t->state_class != state_class::derived) {
      continue;
    }
    auto over = source_->override_for(ctx, i);
    if (!over) {
      return std::unexpected(over.error());
    }
    const provenance *prov = nullptr;
    if (*over) {
      prov = &**over;
    } else {
      if (!origin) {
        auto o = source_->origin(ctx);
        if (!o) {
          return std::unexpected(o.error());
        }
        origin = std::move(*o);
      }
      prov = *origin ? &**origin : nullptr;
    }
    if (prov == nullptr) {
      out.push_back(finding("R-PRV-INCOMPLETE", op.target, op.path, errc::provenance_incomplete,
                            std::format("derived record {} ({}) is written with no provenance on "
                                        "the transaction origin or the op",
                                        op.target.to_string(), t->name)));
    } else if (auto ok = check_provenance(*prov); !ok) {
      out.push_back(finding("R-PRV-INCOMPLETE", op.target, op.path, ok.error().code,
                            std::format("derived record {} ({}): {}", op.target.to_string(),
                                        t->name, ok.error().message)));
    }
  }

  // ARCH1-R42: every class change is in the matrix and, for a promotion, made by an actor with
  // accept on the record's domain.
  auto changes = source_->class_changes(ctx);
  if (!changes) {
    return std::unexpected(changes.error());
  }
  if (changes->empty()) {
    return {};
  }
  const actor *who = actors_->find(ctx.origin().actor);
  if (who == nullptr) {
    out.push_back(diagnostic{.rule = "R-PRV-ACTOR",
                             .severity = severity::error,
                             .record = record_id{},
                             .path = {},
                             .message = std::format("authority_denied: actor {} is not in the "
                                                    "actor directory; unknown actors are denied",
                                                    ctx.origin().actor.to_string()),
                             .stage = validator_stage::invariants});
    return {};
  }
  for (const class_change &c : *changes) {
    std::string domain;
    auto type = type_of(ctx, c.record);
    if (!type) {
      return std::unexpected(type.error());
    }
    if (const type_desc *t = ctx.schemas().find(*type); t != nullptr) {
      if (!t->domain.empty()) {
        domain = t->domain;
      } else if (const domain_spec *spec = domains_->domain_of(t->id); spec != nullptr) {
        domain = spec->name;
      }
    }
    if (auto ok = check_transition(c.from, c.to, *who, domain); !ok) {
      const bool promotion = ok.error().code == errc::promotion_denied;
      out.push_back(finding(promotion ? "R-PRV-PROMOTION" : "R-PRV-TRANSITION", c.record, {},
                            ok.error().code, ok.error().message));
    }
  }
  return {};
}

} // namespace sl::fabric
