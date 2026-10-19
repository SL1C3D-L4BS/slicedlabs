// The generic per-type conformance suite and the gate 1 measure (WO-062): see
// conformance_suite.hpp.

#include "conformance_suite.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric::testing {

namespace {

actor_id conformance_editor() noexcept {
  return actor_id::from(
             derive(id_class::actor, well_known(id_class::namespace_, 1), "conformance/editor"))
      .value_or(actor_id{});
}

// Counts validator registry passes: it runs once per validation (policy stage).
class probe_validator final : public validator {
public:
  explicit probe_validator(std::shared_ptr<std::atomic<std::uint64_t>> runs) noexcept
      : runs_(std::move(runs)) {}
  [[nodiscard]] std::string_view name() const noexcept override { return "conformance.probe"; }
  [[nodiscard]] validator_stage stage() const noexcept override { return validator_stage::policy; }
  [[nodiscard]] result<void> run(const validation_context & /*ctx*/,
                                 std::vector<diagnostic> & /*out*/) const override {
    runs_->fetch_add(1, std::memory_order_relaxed);
    return {};
  }

private:
  std::shared_ptr<std::atomic<std::uint64_t>> runs_;
};

// Counts the records every commit changed (step C6).
class change_counter final : public commit_observer {
public:
  explicit change_counter(std::shared_ptr<std::atomic<std::uint64_t>> changed) noexcept
      : changed_(std::move(changed)) {}
  [[nodiscard]] result<void> on_commit(const commit_event &event,
                                       receipt_builder & /*receipt*/) override {
    changed_->fetch_add(event.changed.size(), std::memory_order_relaxed);
    return {};
  }

private:
  std::shared_ptr<std::atomic<std::uint64_t>> changed_;
};

result<std::uint64_t> stored_objects(const store_backend &store) {
  auto report = store.verify(verify_scope{.hashes = true, .reachability = false, .links = nullptr});
  if (!report) {
    return std::unexpected(report.error());
  }
  return report->objects_hashed;
}

check_outcome pass(std::string name, std::string detail = {}) {
  return check_outcome{
      .check = std::move(name), .applies = true, .passed = true, .detail = std::move(detail)};
}

check_outcome fail(std::string name, std::string detail) {
  return check_outcome{
      .check = std::move(name), .applies = true, .passed = false, .detail = std::move(detail)};
}

check_outcome not_applicable(std::string name, std::string detail) {
  return check_outcome{
      .check = std::move(name), .applies = false, .passed = true, .detail = std::move(detail)};
}

struct run_context {
  workspace *ws = nullptr;
  std::map<std::string, std::vector<record_id>> created;    // record type -> fixture record ids
  std::map<std::string, std::vector<value>> bodies;         // record type -> fixture values
  std::map<std::string, const domain_fixtures *> domain_of; // record type -> its domain entry
};

tx_options options_for(const std::string &subsystem, std::string label,
                       std::optional<provenance> prov = std::nullopt) {
  tx_options o;
  o.origin.actor = conformance_editor();
  o.origin.subsystem = subsystem;
  o.origin.provenance = std::move(prov);
  o.meta.label = std::move(label);
  return o;
}

result<commit_receipt> commit_ops_as(transaction_manager &m, tx_options o,
                                     const std::vector<patch_op> &ops) {
  auto t = m.begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  auto r = t->commit();
  if (!r) {
    std::string why = r.error().message;
    for (const diagnostic &dg : t->diagnostics()) {
      why += std::format("; {} {}", dg.rule, dg.message);
    }
    return sl::make_error(r.error().code, why);
  }
  return r;
}

std::string owner_subsystem(const workspace &ws, const type_desc &td) {
  const domain_spec *spec = ws.domains().domain_of(td.id);
  return spec != nullptr ? std::string(owner_code(spec->owner)) : std::string("02");
}

check_outcome check_mutation(const domain_fixtures &d, const type_desc &td,
                             const std::vector<value> &vals, run_context &ctx) {
  const char *name = "mutation";
  if (!td.is_record()) {
    return not_applicable(name, "not a record type");
  }
  std::vector<record_id> ids;
  std::vector<patch_op> ops;
  for (std::size_t i = 0; i < vals.size(); ++i) {
    result<record_id> id =
        d.record_id_for
            ? d.record_id_for(td, i, vals[i])
            : result<record_id>(derive(td.record_class, well_known(id_class::namespace_, 1),
                                       std::format("conformance/{}/{}", td.name, i)));
    if (!id) {
      return fail(name, "record id: " + id.error().message);
    }
    ids.push_back(*id);
    ops.push_back(op_create(*id, td.id, vals[i]));
  }
  std::optional<provenance> prov = d.provenance_for ? d.provenance_for(td) : std::nullopt;
  const f5::ledger before = f5::read();
  auto r = commit_ops_as(
      ctx.ws->manager(),
      options_for(owner_subsystem(*ctx.ws, td), "conformance create " + td.name, std::move(prov)),
      ops);
  const f5::ledger after = f5::read();
  if (!r) {
    return fail(name, "commit: " + r.error().message);
  }
  if (f5::enabled && (after.mutations <= before.mutations || after.bypassing != before.bypassing)) {
    return fail(name, std::format("F5 ledger delta mutations {} bypassing {}",
                                  after.mutations - before.mutations,
                                  after.bypassing - before.bypassing));
  }
  auto head = ctx.ws->manager().head();
  if (!head) {
    return fail(name, "head: " + head.error().message);
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto e = head->get(ids[i]);
    if (!e || e->type != td.id || e->body != vals[i]) {
      return fail(name, "record " + ids[i].to_string() + " is not live with its fixture body");
    }
  }
  ctx.created[td.name] = ids;
  ctx.bodies[td.name] = vals;
  ctx.domain_of[td.name] = &d;
  return pass(name,
              std::format("{} records, F5 mutations +{} bypassing +{}", ids.size(),
                          after.mutations - before.mutations, after.bypassing - before.bypassing));
}

check_outcome check_inverse(const type_desc &td, run_context &ctx) {
  const char *name = "inverse";
  if (!td.is_record()) {
    return not_applicable(name, "not a record type");
  }
  const auto ids = ctx.created.find(td.name);
  if (ids == ctx.created.end() || ids->second.size() < 2) {
    return fail(name, "the mutation case created no records");
  }
  const std::vector<value> &vals = ctx.bodies[td.name];
  auto head = ctx.ws->manager().head();
  if (!head) {
    return fail(name, "head: " + head.error().message);
  }
  staged_state s(*head);
  const auto body_of = [&s](const record_id &id) -> std::optional<value> {
    auto r = s.read(id);
    if (!r || !r->has_value()) {
      return std::nullopt;
    }
    return (*r)->body;
  };
  patch p;
  p.base = head->revision_digest();
  p.ops = {op_set(ids->second[0], fabric::path{}, vals[1])};
  auto np = normalize(p, s);
  if (!np) {
    return fail(name, "normalize set: " + np.error().message);
  }
  if (auto ok = s.apply(*np); !ok || body_of(ids->second[0]) != vals[1]) {
    return fail(name, "set did not apply");
  }
  if (auto ok = s.apply(invert(*np)); !ok || body_of(ids->second[0]) != vals[0]) {
    return fail(name, "apply(invert(set)) did not restore fixture 0");
  }
  patch q;
  q.base = head->revision_digest();
  q.ops = {op_delete(ids->second[1])};
  auto nq = normalize(q, s);
  if (!nq) {
    return fail(name, "normalize delete: " + nq.error().message);
  }
  if (auto ok = s.apply(*nq); !ok || body_of(ids->second[1]).has_value()) {
    return fail(name, "delete did not apply");
  }
  if (auto ok = s.apply(invert(*nq)); !ok || body_of(ids->second[1]) != vals[1]) {
    return fail(name, "apply(invert(delete)) did not revive fixture 1");
  }
  return pass(name, "set and delete inverted exactly");
}

check_outcome check_migration(const domain_fixtures &d, const type_desc &td) {
  const char *name = "migration";
  if (const compat self = classify(td, td); self != compat::identical) {
    return fail(name, std::format("classify(type, type) is {}", to_string(self)));
  }
  if (!d.previous_types) {
    return pass(name, "identical to itself");
  }
  auto prev = d.previous_types();
  if (!prev) {
    return fail(name, "previous module: " + prev.error().message);
  }
  const auto it =
      std::ranges::find_if(*prev, [&](const type_desc &t) { return t.name == td.name; });
  if (it == prev->end()) {
    return pass(name, "identical to itself; new in this version");
  }
  const compat c = classify(*it, td);
  if (c == compat::forbidden) {
    return fail(name, "the change from the previous version is forbidden");
  }
  return pass(name, std::format("{}.{} -> {}.{} is {}", it->version.major, it->version.minor,
                                td.version.major, td.version.minor, to_string(c)));
}

type_report check_type(const domain_fixtures &d, const type_fixtures &t, run_context &ctx) {
  type_report rep;
  rep.domain = d.name;
  rep.type = t.type;
  rep.fixtures = t.values.size();
  const schema_registry &reg = ctx.ws->schemas();
  const type_desc *td = reg.find(t.type);
  if (td == nullptr) {
    rep.checks.push_back(fail("fixtures", "type is not registered"));
    return rep;
  }
  rep.record = td->is_record();
  rep.checks.push_back(
      t.values.size() >= 2
          ? pass("fixtures", std::format("{} fixtures", t.values.size()))
          : fail("fixtures", std::format("{} fixture(s), needs two", t.values.size())));

  std::vector<value> vals;
  std::vector<std::vector<std::uint8_t>> encodings;
  check_outcome round = pass("round_trip");
  check_outcome canonical = pass("canonical");
  for (const std::string &text : t.values) {
    auto v = from_text(text, td->id, reg);
    if (!v) {
      round = fail("round_trip", "from_text: " + v.error().message + " in " + text);
      break;
    }
    auto bytes = encode(*v);
    if (!bytes) {
      round = fail("round_trip", "encode: " + bytes.error().message);
      break;
    }
    auto back = decode(*bytes, unknown_policy::strict, td->id, reg);
    if (!back || *back != *v) {
      round = fail("round_trip", "strict decode does not give the value back");
      break;
    }
    auto text2 = to_text(*back, td->id, reg);
    auto again =
        text2 ? from_text(*text2, td->id, reg) : result<value>(std::unexpected(text2.error()));
    if (!again || *again != *v) {
      round = fail("round_trip", "to_text does not round trip");
      break;
    }
    auto bytes2 = encode(*back);
    if (canonical.passed && (!bytes2 || *bytes2 != *bytes)) {
      canonical = fail("canonical", "re-encoding the decoded value changes its bytes");
    }
    if (canonical.passed && *text2 != text) {
      canonical =
          fail("canonical", "fixture text is not canonical: " + text + " canonical " + *text2);
    }
    if (canonical.passed && std::ranges::find(encodings, *bytes) != encodings.end()) {
      canonical = fail("canonical", "two fixtures encode alike: " + text);
    }
    encodings.push_back(*bytes);
    vals.push_back(std::move(*v));
  }
  rep.checks.push_back(round);
  rep.checks.push_back(round.passed ? canonical : fail("canonical", "no round trip"));

  const domain_spec *spec = ctx.ws->domains().domain_of(td->id);
  if (td->domain.empty() || spec == nullptr) {
    rep.checks.push_back(fail("ownership", "type has no bound domain (ownership_orphan)"));
  } else {
    const domain_spec *holder = ctx.ws->domains().owner_of(spec->name);
    rep.checks.push_back(
        holder != nullptr && holder->owner == spec->owner
            ? pass("ownership", std::format("{} owned by {}", spec->name, owner_code(spec->owner)))
            : fail("ownership", "the bound domain has no single owner"));
  }

  check_outcome refs = pass("refs");
  for (std::size_t i = 0; i < vals.size(); ++i) {
    std::vector<diagnostic_lite> diags;
    if (auto ok = reg.check(vals[i], td->id, diags); !ok) {
      refs = fail("refs", std::format("fixture {}: {}", i, ok.error().message));
      break;
    }
  }
  rep.checks.push_back(round.passed ? refs : fail("refs", "no round trip"));

  if (round.passed && vals.size() >= 2) {
    rep.checks.push_back(check_mutation(d, *td, vals, ctx));
    rep.checks.push_back(rep.checks.back().passed ? check_inverse(*td, ctx)
                                                  : fail("inverse", "no records"));
  } else {
    rep.checks.push_back(fail("mutation", "no fixtures to write"));
    rep.checks.push_back(fail("inverse", "no fixtures to write"));
  }
  rep.checks.push_back(check_migration(d, *td));
  return rep;
}

} // namespace

bool type_report::passed() const noexcept {
  return !checks.empty() &&
         std::ranges::all_of(checks, [](const check_outcome &c) { return c.passed; });
}

result<conformance_run> run_conformance(const domain_fixture_registry &registry) {
  workspace_options o;
  for (const domain_fixtures &d : registry.domains()) {
    if (auto ok = d.add_types(o.schemas); !ok) {
      return sl::make_error(ok.error().code, d.name + ": types: " + ok.error().message);
    }
  }
  if (auto ok = o.schemas.freeze(); !ok) {
    return std::unexpected(ok.error());
  }
  for (const domain_fixtures &d : registry.domains()) {
    if (auto ok = d.claim(o.domains); !ok) {
      return sl::make_error(ok.error().code, d.name + ": claim: " + ok.error().message);
    }
  }
  if (auto ok = o.domains.bind_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok =
          o.actors.add(actor{.id = conformance_editor(), .kind = actor_kind::human, .grants = {}});
      !ok) {
    return std::unexpected(ok.error());
  }
  o.clock = std::make_shared<manual_clock>(1'700'000'000'000'000);
  o.ids = std::make_shared<deterministic_source>(62, "conformance");
  auto runs = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto changed = std::make_shared<std::atomic<std::uint64_t>>(0);
  o.validators.push_back(std::make_shared<probe_validator>(runs));
  auto ws = workspace::create_memory(std::move(o));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  transaction_manager &m = (*ws)->manager();
  m.add_observer(std::make_shared<change_counter>(changed));
  undo_manager undo(m, conformance_editor());

  auto objects_before = stored_objects((*ws)->store());
  if (!objects_before) {
    return std::unexpected(objects_before.error());
  }
  const std::uint64_t commits_before = m.commits();
  const f5::ledger ledger_before = f5::read();

  conformance_run run;
  run_context ctx{.ws = ws->get(), .created = {}, .bodies = {}, .domain_of = {}};
  for (const domain_fixtures &d : registry.domains()) {
    if (d.prerequisites) {
      auto ops = d.prerequisites((*ws)->schemas());
      if (!ops) {
        return sl::make_error(ops.error().code, d.name + ": prerequisites: " + ops.error().message);
      }
      if (auto r = commit_ops_as(
              m, options_for(d.prerequisites_subsystem, "conformance prerequisites " + d.name),
              *ops);
          !r) {
        return sl::make_error(r.error().code, d.name + ": prerequisites: " + r.error().message);
      }
    }
    for (const type_fixtures &t : d.types) {
      run.types.push_back(check_type(d, t, ctx));
    }
  }

  // The workload: an edit per record type, undo and redo of the last one, a delete per record
  // type (tombstones).
  std::optional<commit_receipt> last;
  for (const auto &[type, ids] : ctx.created) {
    const type_desc *td = (*ws)->schemas().find(type);
    const std::vector<value> &vals = ctx.bodies[type];
    const domain_fixtures *d = ctx.domain_of[type];
    // A derived record carries its F3 provenance on every write (ARCH2-R14).
    std::optional<provenance> prov = d->provenance_for ? d->provenance_for(*td) : std::nullopt;
    auto r = commit_ops_as(
        m, options_for(owner_subsystem(**ws, *td), "conformance edit " + type, std::move(prov)),
        {op_set(ids[0], fabric::path{}, vals[1])});
    if (!r) {
      return sl::make_error(r.error().code, "workload edit of " + type + ": " + r.error().message);
    }
    last = *r;
  }
  if (last) {
    if (auto u = undo.undo(); !u) {
      return sl::make_error(u.error().code, "workload undo: " + u.error().message);
    }
    if (auto rd = undo.redo(); !rd) {
      return sl::make_error(rd.error().code, "workload redo: " + rd.error().message);
    }
  }
  for (const auto &[type, ids] : ctx.created) {
    const type_desc *td = (*ws)->schemas().find(type);
    if (auto r =
            commit_ops_as(m, options_for(owner_subsystem(**ws, *td), "conformance delete " + type),
                          {op_delete(ids[1])});
        !r) {
      return sl::make_error(r.error().code,
                            "workload delete of " + type + ": " + r.error().message);
    }
  }

  const f5::ledger ledger_after = f5::read();
  auto objects_after = stored_objects((*ws)->store());
  if (!objects_after) {
    return std::unexpected(objects_after.error());
  }
  run.counters.ledger = f5::ledger{.mutations = ledger_after.mutations - ledger_before.mutations,
                                   .bypassing = ledger_after.bypassing - ledger_before.bypassing};
  run.counters.commits = m.commits() - commits_before;
  run.counters.objects = *objects_after - *objects_before;
  run.counters.validation_passes = runs->load();
  run.counters.validators = m.validators().size();
  run.counters.changed_records = changed->load();
  run.f2 = (*ws)->domains().f2();
  auto head = m.head();
  if (!head) {
    return std::unexpected(head.error());
  }
  auto gate = measure_gate1(*head, (*ws)->schemas(), (*ws)->domains());
  if (!gate) {
    return std::unexpected(gate.error());
  }
  run.gate1 = std::move(*gate);
  return run;
}

result<gate1_report> measure_gate1(const snapshot &head, const schema_registry &schemas,
                                   const domain_registry &domains) {
  gate1_report rep;
  const bool has_revision = head.revision_digest().has_value();
  auto walked = head.for_each_entry([&](const snapshot_entry &e) -> result<void> {
    ++rep.denominator;
    std::vector<std::string> why;
    const type_desc *td = schemas.find(e.type);
    if (td == nullptr || domains.domain_of(e.type) == nullptr) {
      why.emplace_back("no owner");
    }
    const bool stable = !e.id.is_null() && is_valid(e.id.cls()) && td != nullptr &&
                        (td->record_class == id_class::none || td->record_class == e.id.cls());
    if (!stable) {
      why.emplace_back("no stable id");
    }
    auto stored = head.store().has(e.object);
    if (!has_revision || !stored || !*stored) {
      why.emplace_back("no revision");
    }
    if (!why.empty()) {
      ++rep.numerator;
      std::string text = e.id.to_string() + ":";
      for (const std::string &w : why) {
        text += " " + w;
      }
      rep.offenders.push_back(std::move(text));
    }
    return {};
  });
  if (!walked) {
    return std::unexpected(walked.error());
  }
  return rep;
}

std::string scorecard_input_json(const conformance_run &run) {
  std::size_t failed = 0;
  for (const type_report &t : run.types) {
    failed += t.passed() ? 0 : 1;
  }
  const workload_counters &c = run.counters;
  return std::format("{{\n"
                     "  \"format\": \"sl-fabric-scorecard-input/1\",\n"
                     "  \"types\": {},\n"
                     "  \"types_failed\": {},\n"
                     "  \"f2\": {{\"numerator\": {}, \"denominator\": {}}},\n"
                     "  \"f5\": {{\"enabled\": {}, \"mutations\": {}, \"bypassing\": {}}},\n"
                     "  \"gate1\": {{\"numerator\": {}, \"denominator\": {}}},\n"
                     "  \"commits\": {},\n"
                     "  \"objects\": {},\n"
                     "  \"validation_passes\": {},\n"
                     "  \"validators\": {},\n"
                     "  \"validator_invocations\": {},\n"
                     "  \"changed_records\": {}\n"
                     "}}\n",
                     run.types.size(), failed, run.f2.numerator, run.f2.denominator,
                     c.ledger_enabled ? "true" : "false", c.ledger.mutations, c.ledger.bypassing,
                     run.gate1.numerator, run.gate1.denominator, c.commits, c.objects,
                     c.validation_passes, c.validators, c.validation_passes * c.validators,
                     c.changed_records);
}

std::string scorecard_input_path(std::string_view fixtures_dir) {
  if (const char *env = std::getenv("SL_FABRIC_SCORECARD_INPUT"); env != nullptr && *env != '\0') {
    return env;
  }
  return (std::filesystem::path(std::string(fixtures_dir)) / ".." / ".." / ".." / "build" /
          "fabric" / "scorecard-input.json")
      .lexically_normal()
      .string();
}

result<void> write_text_file(const std::string &path, std::string_view text) {
  const std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
      return sl::make_error(errc::store_io, path + ": " + ec.message());
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  out.flush();
  if (!out.good()) {
    return sl::make_error(errc::store_io, path + ": write failed");
  }
  return {};
}

} // namespace sl::fabric::testing
