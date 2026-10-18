// The built-in validator chain of the transaction manager (WO-050, SL-ARCH-002 ARCH2-R05 and
// `validation_stages`; WO-048 authority, WO-049 provenance). WO-058 appends the identity
// validator to builtin_validators() below (only that line and its include).
//
// Each validator looks only at what the patch changed (the staged changes and the ops), so the
// work is bounded by changed records times validators (SL-ARCH-002 Measures, performance), with
// one exception: a deleted record is checked against every live record for inbound strong refs
// (R-REF-BLOCKED), a scan of the base snapshot that the dependency index of WO-056 can replace.

#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

diagnostic finding(validator_stage stage, std::string rule, const record_id &record, path at,
                   errc code, const std::string &message) {
  return diagnostic{.rule = std::move(rule),
                    .severity = severity::error,
                    .record = record,
                    .path = std::move(at),
                    .message = std::format("{}: {}", to_string(code), message),
                    .stage = stage};
}

// Classes whose ids never name a record of the project model.
bool names_no_record(id_class c) noexcept {
  return c == id_class::none || c == id_class::tx || c == id_class::event ||
         c == id_class::element || c == id_class::operation;
}

// The strong and owned edges a record body holds; nullopt when the body does not fit its type
// (the schema stage reports that).
std::optional<std::vector<edge>> edges_of(const record_id &id, const record_state &r,
                                          const schema_registry &schemas) {
  const type_desc *t = schemas.find(r.type);
  if (t == nullptr || !t->is_record()) {
    return std::nullopt;
  }
  auto e = extract_edges(id, r.body, *t, schemas);
  if (!e) {
    return std::nullopt;
  }
  return std::move(*e);
}

// ---- structural -------------------------------------------------------------------------------

class structural_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.structural"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::structural;
  }

  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override {
    const patch &p = ctx.patch();
    // The state before the patch is the snapshot the manager staged over; tombstones are visible
    // only there (a state_reader reads a retired id as absent).
    const auto *base = dynamic_cast<const snapshot *>(&ctx.before());
    for (std::size_t i = 0; i < p.ops.size(); ++i) {
      const patch_op &op = p.ops[i];
      if (op.kind == op_kind::annotate) {
        continue;
      }
      if (names_no_record(op.target.cls())) {
        out.push_back(finding(
            validator_stage::structural, "R-STR-TARGET", op.target, op.path, errc::invalid_argument,
            std::format("op {} ({}) targets {}, an id of class {} that names no "
                        "record",
                        i, to_string(op.kind), op.target.to_string(), to_string(op.target.cls()))));
        continue;
      }
      if ((op.kind == op_kind::create_record || op.kind == op_kind::retype_record) &&
          op.type.is_null()) {
        out.push_back(finding(validator_stage::structural, "R-STR-TYPE", op.target, op.path,
                              errc::invalid_argument,
                              std::format("op {} ({}) has no type", i, to_string(op.kind))));
      }
      if (op.kind == op_kind::create_record && base != nullptr && p.meta.undo_of.empty()) {
        auto e = base->entry(op.target);
        if (!e) {
          return std::unexpected(e.error());
        }
        if (*e && (*e)->kind == entry_kind::tombstone) {
          out.push_back(finding(validator_stage::structural, "R-STR-TOMBSTONE", op.target, op.path,
                                errc::id_reuse,
                                std::format("op {} creates {}, a retired id; only an undo "
                                            "(undo_of) may revive it",
                                            i, op.target.to_string())));
        }
      }
    }
    return {};
  }
};

// ---- schema -----------------------------------------------------------------------------------

class schema_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.schema"; }
  [[nodiscard]] validator_stage stage() const noexcept override { return validator_stage::schema; }

  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override {
    for (const staged_change &c : ctx.after().changes()) {
      if (!c.after) {
        continue;
      }
      const type_desc *t = ctx.schemas().find(c.after->type);
      if (t == nullptr) {
        out.push_back(finding(validator_stage::schema, "R-SCH-UNKNOWN", c.id, {},
                              errc::schema_unknown,
                              std::format("record {} has the unregistered type {}",
                                          c.id.to_string(), c.after->type.to_string())));
        continue;
      }
      if (!t->is_record()) {
        out.push_back(finding(validator_stage::schema, "R-SCH-RECORD", c.id, {},
                              errc::type_mismatch,
                              std::format("record {} has type {}, which is not a record type",
                                          c.id.to_string(), t->name)));
        continue;
      }
      std::vector<diagnostic_lite> lite;
      auto checked = ctx.schemas().check(c.after->body, c.after->type, lite);
      if (!checked && checked.error().code != errc::type_mismatch) {
        return std::unexpected(checked.error());
      }
      for (diagnostic_lite &d : lite) {
        out.push_back(finding(validator_stage::schema, std::move(d.rule), c.id, std::move(d.path),
                              errc::type_mismatch, std::format("{}: {}", t->name, d.message)));
      }
    }
    return {};
  }
};

// ---- references -------------------------------------------------------------------------------

class references_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.references"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::references;
  }

  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override {
    const schema_registry &schemas = ctx.schemas();
    std::set<record_id> changed;
    std::set<record_id> removed;
    for (const staged_change &c : ctx.after().changes()) {
      changed.insert(c.id);
      if (!c.after) {
        auto before = ctx.before().read(c.id);
        if (!before) {
          return std::unexpected(before.error());
        }
        if (*before) {
          removed.insert(c.id);
        }
        continue;
      }
      // Every strong and owned ref of a written record must be live after the patch.
      const auto edges = edges_of(c.id, *c.after, schemas);
      if (!edges) {
        continue;
      }
      for (const edge &e : *edges) {
        auto target = ctx.after().read(e.to);
        if (!target) {
          return std::unexpected(target.error());
        }
        if (!*target) {
          out.push_back(
              finding(validator_stage::references, "R-REF-DANGLING", c.id, {}, errc::ref_dangling,
                      std::format("{} ref of {} to {}, which is not live", to_string(e.strength),
                                  c.id.to_string(), e.to.to_string())));
        }
      }
    }
    if (removed.empty()) {
      return {};
    }
    // A deleted owner takes its owned children with it in the same patch (owned refs cascade).
    for (const record_id &id : removed) {
      auto before = ctx.before().read(id);
      if (!before) {
        return std::unexpected(before.error());
      }
      const auto edges = edges_of(id, **before, schemas);
      if (!edges) {
        continue;
      }
      for (const edge &e : *edges) {
        if (e.kind != edge_kind::containment) {
          continue;
        }
        auto child = ctx.after().read(e.to);
        if (!child) {
          return std::unexpected(child.error());
        }
        if (*child) {
          out.push_back(finding(validator_stage::references, "R-REF-CASCADE", id, {},
                                errc::ref_dangling,
                                std::format("deleting {} leaves its owned child {} live",
                                            id.to_string(), e.to.to_string())));
        }
      }
    }
    // An unchanged live record must not hold a strong ref to a deleted one (changed records were
    // checked above). The scan reads the base snapshot.
    const auto *base = dynamic_cast<const snapshot *>(&ctx.before());
    if (base == nullptr) {
      return {};
    }
    std::map<record_id, record_id> blocked; // deleted -> first referrer
    auto walked = base->for_each_entry([&](const snapshot_entry &e) -> result<void> {
      if (changed.contains(e.id)) {
        return {};
      }
      const auto edges = edges_of(e.id, record_state{.type = e.type, .body = e.body}, schemas);
      if (!edges) {
        return {};
      }
      for (const edge &x : *edges) {
        if (removed.contains(x.to)) {
          blocked.try_emplace(x.to, e.id);
        }
      }
      return {};
    });
    if (!walked) {
      return std::unexpected(walked.error());
    }
    for (const auto &[id, referrer] : blocked) {
      out.push_back(finding(
          validator_stage::references, "R-REF-BLOCKED", id, {}, errc::ref_dangling,
          std::format("{} is still referenced by {}", id.to_string(), referrer.to_string())));
    }
    return {};
  }
};

// ---- invariants -------------------------------------------------------------------------------

class invariants_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.invariants"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::invariants;
  }

  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override {
    const schema_registry &schemas = ctx.schemas();
    std::map<record_id, record_id> owner_of; // child -> first written owner
    for (const staged_change &c : ctx.after().changes()) {
      if (!c.after) {
        continue;
      }
      const type_desc *t = schemas.find(c.after->type);
      if (t == nullptr || !t->is_record()) {
        continue;
      }
      if (t->record_class != id_class::none && c.id.cls() != t->record_class) {
        out.push_back(finding(
            validator_stage::invariants, "R-INV-ID-CLASS", c.id, {}, errc::id_class_mismatch,
            std::format("{} has class {}, type {} has record class {}", c.id.to_string(),
                        to_string(c.id.cls()), t->name, to_string(t->record_class))));
      }
      const auto edges = edges_of(c.id, *c.after, schemas);
      if (!edges) {
        continue;
      }
      for (const edge &e : *edges) {
        if (e.kind != edge_kind::containment) {
          continue;
        }
        const auto [it, fresh] = owner_of.try_emplace(e.to, c.id);
        if (!fresh && it->second != c.id) {
          out.push_back(finding(validator_stage::invariants, "R-INV-OWNED-TWICE", e.to, {},
                                errc::invariant_violated,
                                std::format("{} is owned by both {} and {}", e.to.to_string(),
                                            it->second.to_string(), c.id.to_string())));
        }
      }
      // The owned forest stays acyclic: no owned chain from this record leads back to it.
      auto cyclic = owns_itself(ctx, c.id, *edges);
      if (!cyclic) {
        return std::unexpected(cyclic.error());
      }
      if (*cyclic) {
        out.push_back(finding(validator_stage::invariants, "R-INV-OWNED-CYCLE", c.id, {},
                              errc::dependency_cycle,
                              std::format("{} owns itself through owned refs", c.id.to_string())));
      }
    }
    return {};
  }

private:
  static result<bool> owns_itself(const validation_context &ctx, const record_id &start,
                                  const std::vector<edge> &first) {
    std::vector<record_id> todo;
    std::set<record_id> seen;
    for (const edge &e : first) {
      if (e.kind == edge_kind::containment) {
        todo.push_back(e.to);
      }
    }
    while (!todo.empty()) {
      const record_id cur = todo.back();
      todo.pop_back();
      if (cur == start) {
        return true;
      }
      if (!seen.insert(cur).second) {
        continue;
      }
      auto r = ctx.after().read(cur);
      if (!r) {
        return std::unexpected(r.error());
      }
      if (!*r) {
        continue;
      }
      const auto edges = edges_of(cur, **r, ctx.schemas());
      if (!edges) {
        continue;
      }
      for (const edge &e : *edges) {
        if (e.kind == edge_kind::containment) {
          todo.push_back(e.to);
        }
      }
    }
    return false;
  }
};

} // namespace

result<std::vector<std::shared_ptr<const validator>>>
builtin_validators(const builtin_validator_env &env) {
  if (env.domains == nullptr || env.actors == nullptr || env.provenance == nullptr) {
    return make_error(errc::invalid_argument,
                      "the built-in validators need the domains, the actors and a provenance "
                      "source");
  }
  std::vector<std::shared_ptr<const validator>> chain;
  chain.push_back(std::make_shared<structural_validator>());
  chain.push_back(std::make_shared<schema_validator>());
  chain.push_back(std::make_shared<authority_validator>(*env.domains, *env.actors));
  chain.push_back(std::make_shared<references_validator>());
  chain.push_back(std::make_shared<invariants_validator>());
  chain.push_back(
      std::make_shared<provenance_validator>(*env.provenance, *env.actors, *env.domains));
  return chain;
}

} // namespace sl::fabric
