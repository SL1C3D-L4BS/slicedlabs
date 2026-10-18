// Newer-minor preservation and deprecation diagnostics (WO-057, SL-ARCH-001 ARCH1-R23,
// ARCH1-R24).

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

std::string version_text(const type_version &v) { return std::format("{}.{}", v.major, v.minor); }

// The type a value of `t` is encoded as: through tagged to its base.
const type_desc *through_tagged(const schema_registry &schemas, const type_desc *t) noexcept {
  for (std::size_t hops = 0;
       t != nullptr && t->kind == type_kind::tagged && t->element && hops < max_value_depth;
       ++hops) {
    t = schemas.find(*t->element);
  }
  return t;
}

// Appends a warning for every deprecated field `v` adds or changes against `old` (null: the
// record or struct is new), recursing into changed struct-typed fields.
void deprecated_writes(const schema_registry &schemas, const type_desc *type, const value &v,
                       const value *old, const fabric::path &at, const record_id &record,
                       std::vector<diagnostic> &out) {
  const type_desc *t = through_tagged(schemas, type);
  if (t == nullptr || t->kind != type_kind::struct_ || v.kind() != value_kind::struct_ ||
      at.size() >= max_value_depth) {
    return;
  }
  for (const field_desc &f : t->fields) {
    const value *now = v.find_field(f.no);
    if (now == nullptr) {
      continue;
    }
    const value *before =
        old != nullptr && old->kind() == value_kind::struct_ ? old->find_field(f.no) : nullptr;
    if (before != nullptr && *before == *now) {
      continue;
    }
    fabric::path p = at;
    p.push_back(path_segment::field(f.no));
    if (f.deprecated_in) {
      out.push_back(diagnostic{
          .rule = "R-SCH-DEPRECATED",
          .severity = severity::warning,
          .record = record,
          .path = p,
          .message = std::format("deprecated: field {} ({}) of {} is deprecated since minor {}",
                                 f.no, f.name, t->name, *f.deprecated_in),
          .stage = validator_stage::schema});
    }
    deprecated_writes(schemas, schemas.find(f.type), *now, before, p, record, out);
  }
}

} // namespace

result<void> check_writable(const snapshot_entry &e, const schema_registry &schemas) {
  const type_desc *t = schemas.find(e.type);
  if (t == nullptr) {
    return make_error(errc::schema_unknown, std::format("record {}: type {} is not registered",
                                                        e.id.to_string(), e.type.to_string()));
  }
  if (e.version.major > t->version.major) {
    return make_error(errc::format_unsupported,
                      std::format("record {} was written by {} {}; this reader knows {}",
                                  e.id.to_string(), t->name, version_text(e.version),
                                  version_text(t->version)));
  }
  if (e.version.major == t->version.major && e.version.minor > t->version.minor) {
    return make_error(errc::read_only_newer,
                      std::format("record {} was written by {} {}; this reader knows {} and "
                                  "keeps it read-only",
                                  e.id.to_string(), t->name, version_text(e.version),
                                  version_text(t->version)));
  }
  return {};
}

result<void> newer_minor_validator::run(const validation_context &ctx,
                                        std::vector<diagnostic> &out) const {
  // The stored version lives only in the record object: the snapshot under the staged state.
  const auto *base = dynamic_cast<const snapshot *>(&ctx.before());
  if (base == nullptr) {
    return {};
  }
  for (const staged_change &c : ctx.after().changes()) {
    auto e = base->get(c.id);
    if (!e) {
      if (e.error().code == errc::not_found) {
        continue; // a new record is written by this reader's version
      }
      return std::unexpected(e.error());
    }
    auto ok = check_writable(*e, ctx.schemas());
    if (ok ||
        (ok.error().code != errc::read_only_newer && ok.error().code != errc::format_unsupported)) {
      continue; // an unregistered type is the schema validator's finding
    }
    out.push_back(
        diagnostic{.rule = "R-SCH-NEWER",
                   .severity = severity::error,
                   .record = c.id,
                   .path = {},
                   .message = std::format("{}: {}", to_string(ok.error().code), ok.error().message),
                   .stage = validator_stage::schema});
  }
  return {};
}

result<void> deprecation_validator::run(const validation_context &ctx,
                                        std::vector<diagnostic> &out) const {
  for (const staged_change &c : ctx.after().changes()) {
    if (!c.after) {
      continue;
    }
    auto before = ctx.before().read(c.id);
    if (!before) {
      return std::unexpected(before.error());
    }
    const value *old = *before && (*before)->type == c.after->type ? &(*before)->body : nullptr;
    deprecated_writes(ctx.schemas(), ctx.schemas().find(c.after->type), c.after->body, old, {},
                      c.id, out);
  }
  return {};
}

} // namespace sl::fabric
