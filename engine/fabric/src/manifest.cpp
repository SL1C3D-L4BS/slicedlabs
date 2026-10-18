// The project manifest: versions, the shape rules, the open-time gate and reading the manifest
// from a snapshot (WO-054, SL-ARCH-001 ARCH1-R32 and ARCH1-R34). The validator and the option
// preparation live in manifest_validator.cpp, project.slp in manifest_locator.cpp.

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t max_ref_name = 255;

bool valid_branch(std::string_view name) noexcept {
  return !name.empty() && name.size() <= max_ref_name && std::ranges::all_of(name, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b >= 0x21 && b <= 0x7E;
  });
}

// "a, b, c".
std::string joined(const std::vector<std::string> &items) {
  std::string out;
  for (const std::string &s : items) {
    out += out.empty() ? s : ", " + s;
  }
  return out;
}

std::unexpected<error> invalid(std::string what) {
  return make_error(errc::manifest_invalid, "manifest: " + std::move(what));
}

// Non-empty and unique entries of a string list ("capability", "target profile").
result<void> check_names(const std::vector<std::string> &names, std::string_view what) {
  std::set<std::string_view> seen;
  for (const std::string &n : names) {
    if (n.empty()) {
      return invalid(std::format("an empty {} id", what));
    }
    if (!seen.insert(n).second) {
      return invalid(std::format("{} '{}' listed twice", what, n));
    }
  }
  return {};
}

result<void> check_pins(const gen::fabric_manifest::package_pins &pins) {
  std::set<record_id> packages;
  std::set<std::string_view> mounts;
  for (const auto &item : pins) {
    const package_pin &pin = item.v;
    if (pin.package.cls() != id_class::package) {
      return invalid(std::format("package pin at '{}': package id {} is not a package id",
                                 pin.mount, pin.package.to_string()));
    }
    if (pin.revision == digest{}) {
      return invalid(std::format("package pin {}: no revision", pin.package.to_string()));
    }
    if (pin.mount.empty()) {
      return invalid(std::format("package pin {}: an empty mount", pin.package.to_string()));
    }
    if (!packages.insert(pin.package).second) {
      return invalid(
          std::format("package {} pinned twice (unique by package id)", pin.package.to_string()));
    }
    if (!mounts.insert(pin.mount).second) {
      return invalid(std::format("mount '{}' pinned twice (unique by mount)", pin.mount));
    }
  }
  return {};
}

} // namespace

record_id manifest_id() noexcept { return well_known(id_class::entity, 1); }

type_id manifest_type() noexcept { return type_id_of(manifest_type_name); }

std::strong_ordering compare(const format_version &a, const format_version &b) noexcept {
  if (const auto c = a.major <=> b.major; std::is_neq(c)) {
    return c;
  }
  return a.minor <=> b.minor;
}

bool in_range(const format_version &v, const version_range &r) noexcept {
  return std::is_lteq(compare(r.at_least, v)) && std::is_lt(compare(v, r.below));
}

std::string to_string(const format_version &v) { return std::format("{}.{}", v.major, v.minor); }

std::string to_string(const version_range &r) {
  return std::format("[{}, {})", to_string(r.at_least), to_string(r.below));
}

result<void> validate_manifest(const project_manifest &m) {
  if (m.project.cls() != id_class::project) {
    return invalid(std::format("project id {} is not a project id", m.project.to_string()));
  }
  if (m.name.empty()) {
    return invalid("an empty project name");
  }
  if (std::is_gt(compare(m.min_reader_format, m.writer_format))) {
    return invalid(std::format("minimum reader {} is above the writer format {}",
                               to_string(m.min_reader_format), to_string(m.writer_format)));
  }
  for (const auto &[id, range] : m.modules) {
    if (id.empty()) {
      return invalid("a required module with an empty id");
    }
    if (std::is_gteq(compare(range.at_least, range.below))) {
      return invalid(std::format("module '{}': range {} admits no version", id, to_string(range)));
    }
  }
  if (auto ok = check_names(m.capabilities, "capability"); !ok) {
    return ok;
  }
  if (auto ok = check_names(m.target_profiles, "target profile"); !ok) {
    return ok;
  }
  if (auto ok = check_pins(m.package_pins); !ok) {
    return ok;
  }
  if (!valid_branch(m.default_branch)) {
    return invalid(std::format("default branch '{}' is not a ref name (1 to 255 bytes of "
                               "0x21..0x7E)",
                               m.default_branch));
  }
  return {};
}

result<open_mode> check_open(const project_manifest &m, const engine_profile &p) {
  if (std::is_gt(compare(m.min_reader_format, p.format))) {
    return make_error(errc::reader_too_old,
                      std::format("manifest: requires a reader of format {} or newer; this engine "
                                  "reads {}",
                                  to_string(m.min_reader_format), to_string(p.format)));
  }
  if (auto ok = validate_manifest(m); !ok) {
    return std::unexpected(ok.error());
  }
  std::vector<std::string> unknown_targets;
  for (const std::string &t : m.target_profiles) {
    if (!p.target_profiles.contains(t)) {
      unknown_targets.push_back("'" + t + "'");
    }
  }
  if (!unknown_targets.empty()) {
    return invalid(std::format("target profile {} required; not an SL-GOV-007 profile of this "
                               "engine (found {} profiles)",
                               joined(unknown_targets), p.target_profiles.size()));
  }
  std::vector<std::string> unmet;
  for (const auto &[id, range] : m.modules) {
    const auto it = p.modules.find(id);
    if (it == p.modules.end()) {
      unmet.push_back(std::format("module '{}' required {}, found none", id, to_string(range)));
    } else if (!in_range(it->second, range)) {
      unmet.push_back(std::format("module '{}' required {}, found {}", id, to_string(range),
                                  to_string(it->second)));
    }
  }
  if (!unmet.empty()) {
    return make_error(errc::module_unmet, "manifest: " + joined(unmet));
  }
  std::vector<std::string> unknown_caps;
  for (const std::string &c : m.capabilities) {
    if (!p.capabilities.contains(c)) {
      unknown_caps.push_back("'" + c + "'");
    }
  }
  if (!unknown_caps.empty()) {
    return make_error(errc::capability_unknown,
                      std::format("manifest: capability {} required; not in the engine's "
                                  "catalog (found {} capabilities)",
                                  joined(unknown_caps), p.capabilities.size()));
  }
  // A newer writer: readable (the minimum reader admits this engine), not writable.
  return std::is_gt(compare(m.writer_format, p.format)) ? open_mode::read_only : open_mode::write;
}

result<project_manifest> read_manifest(const snapshot &s) {
  auto e = s.get(manifest_id());
  if (!e) {
    if (e.error().code == errc::not_found) {
      return make_error(errc::manifest_missing,
                        std::format("no manifest at {}", manifest_id().to_string()));
    }
    return std::unexpected(e.error());
  }
  if (e->type != manifest_type()) {
    return invalid(std::format("the record at {} has type {}, not {}", manifest_id().to_string(),
                               e->type.to_string(), manifest_type_name));
  }
  auto m = project_manifest::from_value(e->body);
  if (!m) {
    return invalid(std::format("the record at {} does not decode: {}", manifest_id().to_string(),
                               m.error().message));
  }
  // Exactly one: the manifest is a document root, so a second one is among the documents.
  auto docs = s.documents();
  if (!docs) {
    return std::unexpected(docs.error());
  }
  for (const record_id &doc : *docs) {
    if (doc == manifest_id()) {
      continue;
    }
    auto other = s.get(doc);
    if (!other) {
      if (other.error().code == errc::not_found) {
        continue;
      }
      return std::unexpected(other.error());
    }
    if (other->type == manifest_type()) {
      return invalid(std::format("a second manifest at {}; the project manifest lives only at {}",
                                 doc.to_string(), manifest_id().to_string()));
    }
  }
  return std::move(*m);
}

result<located_manifest> find_manifest(const store_backend &store) {
  auto refs = store.list_refs();
  if (!refs) {
    return std::unexpected(refs.error());
  }
  std::ranges::sort(*refs, {}, &ref_entry::name);
  const auto read_at = [&store](const digest &head) -> result<project_manifest> {
    auto s = snapshot::open(store, head);
    if (!s) {
      return std::unexpected(s.error());
    }
    return read_manifest(*s);
  };
  for (const ref_entry &r : *refs) {
    auto m = read_at(r.target);
    if (!m) {
      if (m.error().code == errc::manifest_missing) {
        continue;
      }
      return std::unexpected(m.error());
    }
    if (m->default_branch != r.name) {
      const auto it = std::ranges::find(*refs, m->default_branch, &ref_entry::name);
      if (it != refs->end()) {
        auto d = read_at(it->target);
        if (!d) {
          return std::unexpected(d.error());
        }
        return located_manifest{.ref = it->name, .manifest = std::move(*d)};
      }
    }
    return located_manifest{.ref = r.name, .manifest = std::move(*m)};
  }
  return make_error(errc::manifest_missing, std::format("no ref holds a manifest at {} ({} refs)",
                                                        manifest_id().to_string(), refs->size()));
}

result<patch_op> manifest_set_op(const project_manifest &m) {
  auto body = m.to_value();
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_set(manifest_id(), fabric::path{}, std::move(*body));
}

} // namespace sl::fabric
