// The recovery report and corruption class names (WO-055, SL-ARCH-002 ARCH2-R40 and
// `corruption`).

#include <algorithm>
#include <string>
#include <string_view>

#include "sl/fabric/recovery.hpp"

namespace sl::fabric {

std::string_view to_string(corruption_class c) noexcept {
  switch (c) {
  case corruption_class::c1:
    return "C1";
  case corruption_class::c2:
    return "C2";
  case corruption_class::c3:
    return "C3";
  case corruption_class::c4:
    return "C4";
  case corruption_class::c5:
    return "C5";
  case corruption_class::c6:
    return "C6";
  case corruption_class::c7:
    return "C7";
  case corruption_class::c8:
    return "C8";
  case corruption_class::c9:
    return "C9";
  case corruption_class::c10:
    return "C10";
  case corruption_class::c11:
    return "C11";
  case corruption_class::c12:
    return "C12";
  }
  return "C?";
}

std::string_view corruption_name(corruption_class c) noexcept {
  switch (c) {
  case corruption_class::c1:
    return "torn tail";
  case corruption_class::c2:
    return "mid-log CRC mismatch";
  case corruption_class::c3:
    return "broken chain or seq gap";
  case corruption_class::c4:
    return "object hash mismatch";
  case corruption_class::c5:
    return "missing object";
  case corruption_class::c6:
    return "undecodable record";
  case corruption_class::c7:
    return "schema-invalid record";
  case corruption_class::c8:
    return "unknown newer format";
  case corruption_class::c9:
    return "dangling strong reference at rest";
  case corruption_class::c10:
    return "id-class confusion";
  case corruption_class::c11:
    return "stale lock";
  case corruption_class::c12:
    return "corrupt checkpoint";
  }
  return "unknown class";
}

bool recovery_report::blocking() const noexcept {
  return std::ranges::any_of(findings, [](const recovery_finding &f) { return f.blocking; });
}

bool recovery_report::lossless() const noexcept {
  return lost_bytes == 0 && truncated_frames == 0 && orphan_objects == 0 && quarantined.empty() &&
         findings.empty();
}

std::string recovery_report::summary() const {
  std::string out = "recovery: last seq " + std::to_string(last_seq) + ", lost " +
                    std::to_string(lost_bytes) + " bytes, " + std::to_string(truncated_frames) +
                    " frames truncated, " + std::to_string(orphan_objects) + " orphan objects, " +
                    std::to_string(open_intents.size()) + " open intents, " +
                    std::to_string(findings.size()) + " findings";
  for (const recovery_finding &f : findings) {
    out += "\n  ";
    out += f.blocking ? "[blocking] " : "";
    out += f.message;
  }
  return out;
}

} // namespace sl::fabric
