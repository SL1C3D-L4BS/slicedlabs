// project.slp, the project locator (WO-054, SL-ARCH-001 ARCH1-R35, SL-ARCH-002 ARCH2-R28): format,
// store and project id, nothing else.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

std::unexpected<error> invalid(std::string what) {
  return make_error(errc::manifest_invalid,
                    std::string(locator_file_name) + ": " + std::move(what));
}

// A JSON string literal of `s` (UTF-8 passes through; quotes, backslashes and controls escaped).
std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (b < 0x20) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(b));
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

result<void> check_store(std::string_view store) {
  if (store.empty()) {
    return invalid("an empty store path");
  }
  const stdfs::path p{std::string(store)};
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
    return invalid(std::format("store '{}' is not relative to the project root", store));
  }
  for (const stdfs::path &part : p) {
    if (part == "..") {
      return invalid(std::format("store '{}' leaves the project root", store));
    }
  }
  return {};
}

std::span<const std::uint8_t> text_bytes(std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): chars viewed as their bytes
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

} // namespace

result<project_locator> parse_locator(std::string_view text) {
  auto doc = parse_json(text);
  if (!doc) {
    return invalid("not JSON: " + doc.error().message);
  }
  if (doc->kind != json_value::kind_type::object) {
    return invalid("not a JSON object");
  }
  constexpr std::array<std::string_view, 3> keys = {"format", "store", "project"};
  for (const json_member &m : doc->members) {
    if (std::ranges::find(keys, m.key) == keys.end()) {
      return invalid(std::format("key '{}' is not allowed (only format, store and project: the "
                                 "locator holds no project facts)",
                                 m.key));
    }
    if (m.v.kind != json_value::kind_type::string) {
      return invalid(std::format("key '{}' is not a string", m.key));
    }
  }
  for (const std::string_view k : keys) {
    if (doc->find(k) == nullptr) {
      return invalid(std::format("key '{}' is missing", k));
    }
  }
  project_locator out;
  out.format = doc->find("format")->text;
  if (out.format != locator_format) {
    return make_error(errc::format_unsupported,
                      std::format("{}: format '{}', this engine reads '{}'", locator_file_name,
                                  out.format, locator_format));
  }
  out.store = doc->find("store")->text;
  if (auto ok = check_store(out.store); !ok) {
    return std::unexpected(ok.error());
  }
  auto project = record_id::parse(doc->find("project")->text);
  if (!project) {
    return invalid("project: " + project.error().message);
  }
  if (project->cls() != id_class::project) {
    return invalid(std::format("project {} is not a project id", project->to_string()));
  }
  out.project = *project;
  return out;
}

std::string format_locator(const project_locator &l) {
  return std::format("{{\"format\": {}, \"store\": {}, \"project\": {}}}\n", json_string(l.format),
                     json_string(l.store), json_string(l.project.to_string()));
}

result<project_locator> read_locator(vfs &fs, const std::filesystem::path &root) {
  const stdfs::path file = root / locator_file_name;
  const result<bool> present = fs.exists(file);
  if (!present) {
    return std::unexpected(present.error());
  }
  if (!*present) {
    return make_error(errc::not_found, file.string() + ": no project locator");
  }
  auto bytes = read_file(fs, file);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const std::string text(bytes->begin(), bytes->end());
  return parse_locator(text);
}

result<void> write_locator(vfs &fs, const std::filesystem::path &root, const project_locator &l,
                           durability sync) {
  if (auto ok = check_store(l.store); !ok) {
    return ok;
  }
  if (l.format != locator_format || l.project.cls() != id_class::project) {
    return invalid("a locator needs format " + std::string(locator_format) + " and a project id");
  }
  const stdfs::path file = root / locator_file_name;
  stdfs::path tmp = file;
  tmp += ".tmp";
  const std::string text = format_locator(l);
  {
    result<vfs_file> f = fs.open(tmp, open_create);
    if (!f) {
      return std::unexpected(f.error());
    }
    if (auto ok = f->append(text_bytes(text)); !ok) {
      return std::unexpected(ok.error());
    }
    if (sync != durability::none) {
      if (auto ok = f->fsync(); !ok) {
        return ok;
      }
    }
  }
  if (auto ok = fs.rename(tmp, file); !ok) {
    return ok;
  }
  if (sync == durability::none) {
    return {};
  }
  return fs.sync_dir(root.empty() ? stdfs::path(".") : root);
}

} // namespace sl::fabric
