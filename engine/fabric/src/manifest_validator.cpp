// The manifest validator and the workspace option preparation (WO-054, SL-ARCH-001 ARCH1-R32 and
// ARCH1-R33): one manifest, at the well-known id, well formed, with unique package pins.

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/gen/fabric.manifest.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

namespace {

diagnostic finding(std::string_view rule, const record_id &record, errc code,
                   const std::string &message) {
  return diagnostic{.rule = std::string(rule),
                    .severity = severity::error,
                    .record = record,
                    .path = {},
                    .message = std::format("{}: {}", to_string(code), message),
                    .stage = validator_stage::invariants};
}

// R-MAN-PIN for the pin findings of validate_manifest, R-MAN-SHAPE for the rest.
std::string_view shape_rule(std::string_view message) {
  return message.contains("pinned twice") ? "R-MAN-PIN" : "R-MAN-SHAPE";
}

class manifest_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return manifest_validator_name; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::invariants;
  }

  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override {
    const type_id type = manifest_type();
    const record_id where = manifest_id();
    for (const staged_change &c : ctx.after().changes()) {
      if (c.id != where) {
        if (c.after && c.after->type == type) {
          out.push_back(finding("R-MAN-ONE", c.id, errc::manifest_invalid,
                                std::format("a second manifest at {}; the project manifest lives "
                                            "only at {}",
                                            c.id.to_string(), where.to_string())));
        }
        continue;
      }
      if (!c.after) {
        auto before = ctx.before().read(where);
        if (!before) {
          return std::unexpected(before.error());
        }
        if (before->has_value()) {
          out.push_back(finding("R-MAN-MISSING", where, errc::manifest_missing,
                                "the project manifest cannot be deleted"));
        }
        continue;
      }
      if (c.after->type != type) {
        out.push_back(finding("R-MAN-TYPE", where, errc::manifest_invalid,
                              std::format("the manifest id holds type {}, not {}",
                                          c.after->type.to_string(), manifest_type_name)));
        continue;
      }
      auto m = project_manifest::from_value(c.after->body);
      if (!m) {
        out.push_back(finding("R-MAN-SHAPE", where, errc::manifest_invalid,
                              "the manifest does not decode: " + m.error().message));
        continue;
      }
      if (auto ok = validate_manifest(*m); !ok) {
        out.push_back(finding(shape_rule(ok.error().message), where, errc::manifest_invalid,
                              ok.error().message));
      }
      if (auto ok = check_project_kept(ctx, *m, out); !ok) {
        return ok;
      }
    }
    return {};
  }

private:
  // R-MAN-PROJECT: the project id of an existing manifest never changes.
  static result<void> check_project_kept(const validation_context &ctx,
                                         const project_manifest &after,
                                         std::vector<diagnostic> &out) {
    auto before = ctx.before().read(manifest_id());
    if (!before) {
      return std::unexpected(before.error());
    }
    if (!before->has_value() || (*before)->type != manifest_type()) {
      return {};
    }
    auto old = project_manifest::from_value((*before)->body);
    if (old && old->project != after.project) {
      out.push_back(finding("R-MAN-PROJECT", manifest_id(), errc::manifest_invalid,
                            std::format("the project id changed from {} to {}",
                                        old->project.to_string(), after.project.to_string())));
    }
    return {};
  }
};

} // namespace

std::shared_ptr<const validator> make_manifest_validator() {
  return std::make_shared<manifest_validator>();
}

result<void> prepare_manifest_options(workspace_options &options) {
  if (options.schemas.find(manifest_type()) == nullptr) {
    if (options.schemas.frozen()) {
      return make_error(
          errc::registry_frozen,
          std::format("the schema registry is frozen without {}", manifest_type_name));
    }
    if (auto ok = gen::fabric_manifest::register_types(options.schemas); !ok) {
      return ok;
    }
  }
  if (options.domains.owner_of(manifest_domain) == nullptr) {
    if (options.domains.frozen()) {
      return make_error(errc::registry_frozen,
                        std::format("the domain registry is frozen without {}", manifest_domain));
    }
    if (auto ok = options.domains.claim(domain_spec{.name = std::string(manifest_domain),
                                                    .owner = subsystem::fabric,
                                                    .state_classes = {state_class::document},
                                                    .types = {}});
        !ok) {
      return ok;
    }
  }
  if (options.domains.domain_of(manifest_type()) == nullptr) {
    if (options.domains.frozen()) {
      return make_error(
          errc::registry_frozen,
          std::format("the domain registry is frozen with {} unbound", manifest_type_name));
    }
    return options.domains.bind_types(options.schemas);
  }
  return {};
}

} // namespace sl::fabric
