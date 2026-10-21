// snapshot save and load (WO-095, SL-ARCH-003 ARCH3-R147, `failpoints` RFP8, ADR-0015 pattern):
// save writes the bytes through file_access::atomic_write (temp file, fsync, rename, directory
// fsync), so a crash after any storage primitive leaves the old or the new file; load reads them
// back and checks them as snapshot::from_bytes.

#include "sl/runtime/snapshot.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/platform.hpp"

#include <string_view>
#include <utility>

namespace sl::runtime {

result<void> save(const file_access &files, std::string_view path, const snapshot &s) {
#if SL_RUNTIME_FAULTS
  if (visit(failpoint::rfp8).fire) {
    return make_error(errc::io_error, "snapshot::save: RFP8, a simulated crash before the write");
  }
#endif
  return files.atomic_write(path, s.bytes());
}

result<snapshot> load(const file_access &files, std::string_view path) {
  auto bytes = files.read(path);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return snapshot::from_bytes(std::move(*bytes));
}

} // namespace sl::runtime
