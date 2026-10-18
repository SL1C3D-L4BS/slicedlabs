// The shared verify walk of store backends (WO-042, SL-ARCH-002 ARCH2-R02 and R44).

#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric {

namespace {

bool is_finding(errc code) noexcept {
  return code == errc::object_corrupt || code == errc::object_missing;
}

} // namespace

result<verify_report> verify_store(const store_backend &store, const verify_scope &scope,
                                   std::span<const digest> stored) noexcept {
  verify_report report;
  std::map<std::pair<digest, errc>, verify_finding> findings;
  const auto note = [&findings](const error &e, const digest &id, std::string_view ref) {
    findings.try_emplace(
        {id, e.code},
        verify_finding{.code = e.code, .id = id, .ref = std::string(ref), .message = e.message});
  };

  if (scope.hashes) {
    for (const digest &id : stored) {
      ++report.objects_hashed;
      const result<object> obj = store.get(id);
      if (!obj) {
        if (!is_finding(obj.error().code)) {
          return std::unexpected(obj.error());
        }
        note(obj.error(), id, {});
      }
    }
  }

  if (scope.reachability) {
    const result<std::vector<ref_entry>> refs = store.list_refs();
    if (!refs) {
      return std::unexpected(refs.error());
    }
    std::set<digest> seen;
    std::vector<digest> pending;
    for (const ref_entry &ref : *refs) {
      ++report.refs_walked;
      pending.push_back(ref.target);
      while (!pending.empty()) {
        const digest id = pending.back();
        pending.pop_back();
        if (!seen.insert(id).second) {
          continue;
        }
        ++report.objects_reached;
        const result<object> obj = store.get(id);
        if (!obj) {
          if (!is_finding(obj.error().code)) {
            return std::unexpected(obj.error());
          }
          note(obj.error(), id, ref.name);
          continue;
        }
        if (scope.links != nullptr) {
          if (result<void> linked = scope.links->append_links(*obj, pending); !linked) {
            return std::unexpected(linked.error());
          }
        }
      }
    }
  }

  report.findings.reserve(findings.size());
  for (auto &entry : findings) {
    report.findings.push_back(std::move(entry.second));
  }
  return report;
}

} // namespace sl::fabric
