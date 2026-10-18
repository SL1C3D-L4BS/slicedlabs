// Identity lifetime: tombstone reads, stable id helpers, import records and the identity
// validator (WO-058, SL-ARCH-001 ARCH1-R04 to ARCH1-R07, ARCH1-R17, ARCH1-R65; ADR-0011).

#include "sl/fabric/identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

namespace {

diagnostic finding(std::string rule, const record_id &record, errc code,
                   const std::string &message) {
  return diagnostic{.rule = std::move(rule),
                    .severity = severity::error,
                    .record = record,
                    .path = {},
                    .message = std::format("{}: {}", to_string(code), message),
                    .stage = validator_stage::invariants};
}

bool fits_u32(std::string_view s) noexcept {
  return s.size() < std::numeric_limits<std::uint32_t>::max();
}

bool is_zero(const digest &d) noexcept {
  return std::ranges::all_of(d.bytes, [](std::uint8_t b) { return b == 0; });
}

bool may_parent_instance(id_class c) noexcept {
  return c == id_class::instance || c == id_class::entity || c == id_class::asset;
}

} // namespace

std::string_view to_string(id_lifetime l) noexcept {
  switch (l) {
  case id_lifetime::unborn:
    return "unborn";
  case id_lifetime::live:
    return "live";
  case id_lifetime::tombstoned:
    return "tombstoned";
  }
  return "unknown";
}

bool names_record(id_class c) noexcept {
  return is_valid(c) && c != id_class::tx && c != id_class::event && c != id_class::element &&
         c != id_class::operation;
}

result<id_lifetime> lifetime_of(const snapshot &s, const record_id &id) {
  auto e = s.entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  if (!*e) {
    return id_lifetime::unborn;
  }
  return (*e)->kind == entry_kind::tombstone ? id_lifetime::tombstoned : id_lifetime::live;
}

result<std::optional<tombstone>> tombstone_of(const snapshot &s, const record_id &id) {
  auto e = s.entry(id);
  if (!e) {
    return std::unexpected(e.error());
  }
  if (!*e || (*e)->kind != entry_kind::tombstone) {
    return std::optional<tombstone>{};
  }
  auto o = s.store().get((*e)->target);
  if (!o) {
    return std::unexpected(o.error());
  }
  auto t = decode_tombstone(*o);
  if (!t) {
    return std::unexpected(t.error());
  }
  if (t->id != id) {
    return make_error(errc::object_corrupt,
                      std::format("the tombstone leaf of {} names a tombstone object of {}",
                                  id.to_string(), t->id.to_string()));
  }
  return std::optional<tombstone>(*t);
}

bool may_revive(const patch_meta &meta, const record_id &retired_by) noexcept {
  return !retired_by.is_null() &&
         std::ranges::any_of(meta.undo_of, [&](const tx_id &t) { return t.id() == retired_by; });
}

result<instance_id> instance_id_for(const record_id &parent, const record_id &source) {
  if (parent.is_null() || source.is_null()) {
    return make_error(errc::invalid_argument, "an instance id needs a parent and a source id");
  }
  if (!may_parent_instance(parent.cls())) {
    return make_error(errc::id_class_mismatch,
                      std::format("the parent of an instance is an instance or a document id, "
                                  "not {} ({})",
                                  parent.to_string(), to_string(parent.cls())));
  }
  if (!may_parent_instance(source.cls())) {
    return make_error(errc::id_class_mismatch,
                      std::format("the source of an instance is an entity, asset or instance id, "
                                  "not {} ({})",
                                  source.to_string(), to_string(source.cls())));
  }
  return instance_id::from(derive(id_class::instance, parent, source.to_string()));
}

result<derived_id> derived_id_for(const digest &key) {
  if (key.algo != hash_algo::sha256) {
    return make_error(errc::invalid_argument, "a derivation key is a SHA-256 digest");
  }
  if (is_zero(key)) {
    return make_error(errc::invalid_argument, "the all-zero digest is no derivation key");
  }
  return derived_id::from(
      derive(id_class::derived, well_known(id_class::namespace_, 1), key.to_hex()));
}

result<derived_id> derived_id_for(const provenance &p) {
  return derivation_key(p).and_then([](const digest &k) { return derived_id_for(k); });
}

result<record_id> imported_id_for(id_class c, std::string_view system,
                                  std::string_view external_id) {
  if (!names_record(c)) {
    return make_error(errc::id_class_mismatch,
                      std::format("an imported id of class {} would name no record", to_string(c)));
  }
  if (system.empty() || external_id.empty()) {
    return make_error(errc::invalid_argument,
                      "an imported id needs a system name and an external id");
  }
  if (!fits_u32(system) || !fits_u32(external_id)) {
    return make_error(errc::invalid_argument, "an imported id input is 2^32 bytes or longer");
  }
  return imported(c, system, external_id);
}

result<import_record> make_import_record(id_class c, std::string_view system,
                                         std::string_view external_id, std::string_view locator,
                                         const digest &content) {
  auto id = imported_id_for(c, system, external_id);
  if (!id) {
    return std::unexpected(id.error());
  }
  import_record r{.system = std::string(system),
                  .external_id = std::string(external_id),
                  .locator = std::string(locator),
                  .content = content,
                  .record = *id};
  if (auto ok = check_import_record(r); !ok) {
    return std::unexpected(ok.error());
  }
  return r;
}

result<void> check_import_record(const import_record &r) {
  if (r.system.empty() || r.external_id.empty()) {
    return make_error(errc::invalid_argument, "import record: empty system or external id");
  }
  if (r.locator.empty()) {
    return make_error(errc::invalid_argument, "import record: empty locator");
  }
  if (is_zero(r.content)) {
    return make_error(errc::invalid_argument, "import record: no content digest");
  }
  if (!names_record(r.record.cls()) || r.record.scheme() != mint_scheme::imported ||
      !fits_u32(r.system) || !fits_u32(r.external_id) ||
      r.record != imported(r.record.cls(), r.system, r.external_id)) {
    return make_error(errc::invalid_argument,
                      std::format("import record: {} is not the imported id of ({}, {})",
                                  r.record.to_string(), r.system, r.external_id));
  }
  return {};
}

result<void> identity_validator::run(const validation_context &ctx,
                                     std::vector<diagnostic> &out) const {
  const schema_registry &schemas = ctx.schemas();
  // Tombstones are visible only in the base snapshot: a state_reader reads a retired id as absent.
  const auto *base = dynamic_cast<const snapshot *>(&ctx.before());
  const patch_meta &meta = ctx.patch().meta;
  for (const staged_change &c : ctx.after().changes()) {
    if (!c.after) {
      continue; // a delete: the revision builder writes the tombstone
    }
    const type_desc *t = schemas.find(c.after->type);
    if (t != nullptr && t->is_record() && t->record_class != id_class::none &&
        c.id.cls() != t->record_class) {
      out.push_back(
          finding("R-INV-ID-CLASS", c.id, errc::id_class_mismatch,
                  std::format("{} has class {}, type {} has record class {}", c.id.to_string(),
                              to_string(c.id.cls()), t->name, to_string(t->record_class))));
    }
    if (base == nullptr) {
      continue;
    }
    auto tomb = tombstone_of(*base, c.id);
    if (!tomb) {
      return std::unexpected(tomb.error());
    }
    if (*tomb && !may_revive(meta, (*tomb)->retired_by)) {
      out.push_back(finding(
          "R-INV-ID-REUSE", c.id, errc::id_reuse,
          std::format("{} was retired by {}; only the undo of that transaction (undo_of) may "
                      "revive it",
                      c.id.to_string(), (*tomb)->retired_by.to_string())));
    }
  }
  return {};
}

} // namespace sl::fabric
