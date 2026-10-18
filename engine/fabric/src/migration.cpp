// Migration steps, plans and the migration transaction (WO-057, SL-ARCH-001 ARCH1-R21, ARCH1-R25).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

std::string version_text(const type_version &v) { return std::format("{}.{}", v.major, v.minor); }

bool takes_field(step_kind k) noexcept {
  return k == step_kind::add_field || k == step_kind::remove_field || k == step_kind::retype;
}

bool takes_converter(step_kind k) noexcept {
  return k == step_kind::retype || k == step_kind::native;
}

fabric::path path_of(const migration_step &s) {
  return takes_field(s.kind) ? fabric::path{path_segment::field(s.field)} : fabric::path{};
}

std::unexpected<error> failed(const planned_record &r, const fabric::path &p,
                              std::string_view why) {
  return make_error(errc::migration_failed,
                    std::format("record {} (type {} {} -> {}) at {}: {}", r.id.to_string(),
                                r.type.to_string(), version_text(r.from), version_text(r.to),
                                to_string(p), why));
}

// One step over one body (the step kinds of migration.hpp).
result<value> apply_step(const migration_step &s, const value &body, const migration_plan &plan) {
  if (s.kind == step_kind::identity) {
    return body;
  }
  const migration_converter *conv = nullptr;
  if (takes_converter(s.kind)) {
    const auto it = plan.converters.find(s.converter);
    if (it == plan.converters.end() || !it->second || !*it->second) {
      return make_error(errc::not_found, "no converter named " + s.converter);
    }
    conv = it->second.get();
  }
  if (s.kind == step_kind::native) {
    return (*conv)(body);
  }
  if (body.kind() != value_kind::struct_) {
    return make_error(errc::type_mismatch, "the body is not a struct");
  }
  const fabric::path p{path_segment::field(s.field)};
  switch (s.kind) {
  case step_kind::add_field:
    if (body.has_field(s.field) || !s.fill) {
      return body;
    }
    return body.with(p, *s.fill);
  case step_kind::remove_field:
    return body.without(p);
  case step_kind::retype: {
    const value *v = body.find_field(s.field);
    if (v == nullptr) {
      return body;
    }
    auto converted = (*conv)(*v);
    if (!converted) {
      return std::unexpected(converted.error());
    }
    return body.with(p, std::move(*converted));
  }
  default:
    return make_error(errc::invalid_argument, "unknown step kind");
  }
}

// struct {1 "fabric.migration", 2 record count, 3 loss notes}: the note of the annotate op.
result<value> migration_note(const migration_plan &plan) {
  auto label = value::string("fabric.migration");
  if (!label) {
    return std::unexpected(label.error());
  }
  std::vector<value> notes;
  for (const std::string &loss : plan.losses()) {
    auto s = value::string(loss);
    if (!s) {
      return std::unexpected(s.error());
    }
    notes.push_back(std::move(*s));
  }
  auto list = value::list(std::move(notes));
  if (!list) {
    return std::unexpected(list.error());
  }
  return value::struct_({field{.no = 1, .v = std::move(*label)},
                         field{.no = 2, .v = value::uint(plan.records.size())},
                         field{.no = 3, .v = std::move(*list)}});
}

} // namespace

std::string_view to_string(step_kind k) noexcept {
  switch (k) {
  case step_kind::add_field:
    return "add_field";
  case step_kind::remove_field:
    return "remove_field";
  case step_kind::retype:
    return "retype";
  case step_kind::identity:
    return "identity";
  case step_kind::native:
    return "native";
  }
  return "unknown";
}

// ---- migration_registry -------------------------------------------------------------------

result<void> migration_registry::add_converter(std::string name, migration_converter fn) {
  if (name.empty() || !fn) {
    return make_error(errc::invalid_argument, "a converter needs a name and a function");
  }
  if (converters_.contains(name)) {
    return make_error(errc::already_exists, "converter " + name + " is registered");
  }
  converters_.emplace(std::move(name), std::make_shared<const migration_converter>(std::move(fn)));
  return {};
}

result<void> migration_registry::add(migration_step step) {
  const std::string what =
      std::format("step {} of {} {} -> {}", to_string(step.kind), step.type.to_string(),
                  version_text(step.from), version_text(step.to));
  if (step.type.is_null()) {
    return make_error(errc::invalid_argument, what + ": no type");
  }
  if (step.to.major <= step.from.major) {
    return make_error(errc::invalid_argument, what + ": a step must raise the major version");
  }
  if (takes_field(step.kind) != (step.field != 0)) {
    return make_error(errc::invalid_argument, what + ": field number does not fit the kind");
  }
  if (step.fill && step.kind != step_kind::add_field) {
    return make_error(errc::invalid_argument, what + ": only add_field takes a fill value");
  }
  if (takes_converter(step.kind) != !step.converter.empty()) {
    return make_error(errc::invalid_argument, what + ": converter name does not fit the kind");
  }
  if (!step.converter.empty() && !converters_.contains(step.converter)) {
    return make_error(errc::not_found, what + ": no converter named " + step.converter);
  }
  if (step.kind == step_kind::remove_field && step.loss.empty()) {
    return make_error(errc::migration_lossy, what + ": removing a field needs a loss note");
  }
  auto &hop = hops_[{step.type, step.from.major}];
  if (!hop.empty() && (hop.front().from != step.from || hop.front().to != step.to)) {
    return make_error(errc::already_exists,
                      std::format("{}: the hop from major {} already goes {} -> {}", what,
                                  step.from.major, version_text(hop.front().from),
                                  version_text(hop.front().to)));
  }
  hop.push_back(std::move(step));
  return {};
}

std::span<const migration_step> migration_registry::steps_from(const type_id &type,
                                                               std::uint32_t major) const noexcept {
  const auto it = hops_.find({type, major});
  if (it == hops_.end()) {
    return {};
  }
  return it->second;
}

std::shared_ptr<const migration_converter>
migration_registry::converter(std::string_view name) const noexcept {
  const auto it = converters_.find(name);
  return it == converters_.end() ? nullptr : it->second;
}

result<compat> migration_registry::check_change(const type_desc &from, const type_desc &to) const {
  const compat c = classify(from, to);
  if (c == compat::forbidden) {
    return make_error(errc::schema_invalid,
                      std::format("{} {} -> {}: a forbidden change", from.name,
                                  version_text(from.version), version_text(to.version)));
  }
  if (c == compat::backward_only || c == compat::breaking) {
    const auto hop = steps_from(from.id, from.version.major);
    if (hop.empty() || hop.front().to.major != to.version.major ||
        hop.front().from.minor < from.version.minor) {
      return make_error(errc::migration_missing_step,
                        std::format("{} {} -> {} is {} and needs a migration step", from.name,
                                    version_text(from.version), version_text(to.version),
                                    to_string(c)));
    }
  }
  return c;
}

// ---- plans --------------------------------------------------------------------------------

std::vector<std::string> migration_plan::losses() const {
  std::vector<std::string> out;
  for (const planned_record &r : records) {
    for (const migration_step &s : r.steps) {
      if (!s.loss.empty()) {
        out.push_back(s.loss);
      }
    }
  }
  std::ranges::sort(out);
  const auto [first, last] = std::ranges::unique(out);
  out.erase(first, last);
  return out;
}

result<migration_plan> plan_migration(const snapshot &s, const schema_registry &target,
                                      const migration_registry &steps) {
  migration_plan plan;
  plan.base = s.revision_digest();
  auto set = schema_set_digest(target);
  if (!set) {
    return std::unexpected(set.error());
  }
  plan.target_schema_set = *set;
  auto walked = s.for_each_entry([&](const snapshot_entry &e) -> result<void> {
    const type_desc *t = target.find(e.type);
    if (t == nullptr) {
      return make_error(errc::schema_unknown,
                        std::format("record {}: type {} is not in the target registry",
                                    e.id.to_string(), e.type.to_string()));
    }
    const type_version want = t->version;
    if (e.version.major == want.major) {
      if (e.version.minor > want.minor) {
        plan.preserved.push_back(e.id); // ARCH1-R23: kept byte-identical
      }
      return {};
    }
    if (e.version.major > want.major) {
      return make_error(errc::format_unsupported,
                        std::format("record {}: {} {} is newer than the target {}",
                                    e.id.to_string(), t->name, version_text(e.version),
                                    version_text(want)));
    }
    planned_record r{.id = e.id, .type = e.type, .from = e.version, .to = want, .steps = {}};
    type_version cur = e.version;
    while (cur.major < want.major) {
      const auto hop = steps.steps_from(e.type, cur.major);
      if (hop.empty() || hop.front().from.minor < cur.minor) {
        return make_error(errc::migration_missing_step,
                          std::format("record {}: no migration step for {} from {} (target {})",
                                      e.id.to_string(), t->name, version_text(cur),
                                      version_text(want)));
      }
      for (const migration_step &step : hop) {
        if (!step.converter.empty()) {
          plan.converters.insert_or_assign(step.converter, steps.converter(step.converter));
        }
        r.steps.push_back(step);
      }
      cur = hop.front().to;
    }
    if (cur.major != want.major || cur.minor > want.minor) {
      return make_error(errc::migration_missing_step,
                        std::format("record {}: the steps for {} end at {}, not at the target {}",
                                    e.id.to_string(), t->name, version_text(cur),
                                    version_text(want)));
    }
    plan.records.push_back(std::move(r));
    return {};
  });
  if (!walked) {
    return std::unexpected(walked.error());
  }
  return plan;
}

actor_id migration_system_actor() noexcept {
  return actor_id::from(
             derive(id_class::actor, well_known(id_class::namespace_, 1), "fabric.migration"))
      .value_or(actor_id{});
}

result<commit_receipt> run_migration(transaction_manager &m, const migration_plan &plan,
                                     const tx_origin &origin) {
  auto set = schema_set_digest(m.schemas());
  if (!set) {
    return std::unexpected(set.error());
  }
  if (*set != plan.target_schema_set) {
    return make_error(errc::invalid_argument,
                      "the manager's schema registry is not the plan's target");
  }
  tx_options o;
  o.origin = origin;
  o.origin.actor = migration_system_actor();
  o.origin.epistemic = epistemic_class::calculated;
  o.ref = plan.ref;
  o.base = plan.base;
  o.policy = commit_policy::reject_stale;
  o.meta.label = std::format("fabric.migration: {} record(s)", plan.records.size());
  auto t = m.begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const planned_record &r : plan.records) {
    auto cur = t->view().read(r.id);
    if (!cur) {
      return std::unexpected(cur.error());
    }
    if (!*cur || (*cur)->type != r.type) {
      return failed(r, {}, "the record is not live with its planned type");
    }
    value body = (*cur)->body;
    for (const migration_step &s : r.steps) {
      auto next = apply_step(s, body, plan);
      if (!next) {
        return failed(r, path_of(s),
                      std::format("step {}{}{}: {}: {}", to_string(s.kind),
                                  s.converter.empty() ? "" : " ", s.converter,
                                  to_string(next.error().code), next.error().message));
      }
      body = std::move(*next);
    }
    if (auto ok = t->stage(op_retype(r.id, r.type, std::move(body))); !ok) {
      return failed(r, {}, std::format("{}: {}", to_string(ok.error().code), ok.error().message));
    }
  }
  auto note = migration_note(plan);
  if (!note) {
    return std::unexpected(note.error());
  }
  if (auto ok = t->stage(op_annotate(std::move(*note))); !ok) {
    return std::unexpected(ok.error());
  }
  auto receipt = t->commit();
  if (!receipt && receipt.error().code == errc::validation_failed) {
    const auto diags = t->diagnostics();
    const auto first = std::ranges::find_if(
        diags, [](const diagnostic &d) { return d.severity == severity::error; });
    if (first != diags.end()) {
      const auto rec = std::ranges::find_if(
          plan.records, [&](const planned_record &r) { return r.id == first->record; });
      std::string why =
          std::format("{} [{}]: {}", first->rule, to_string(first->stage), first->message);
      if (rec != plan.records.end()) {
        return failed(*rec, first->path, why);
      }
      return make_error(errc::migration_failed,
                        std::format("record {} at {}: {}", first->record.to_string(),
                                    to_string(first->path), why));
    }
    return make_error(errc::migration_failed, receipt.error().message);
  }
  return receipt;
}

} // namespace sl::fabric
