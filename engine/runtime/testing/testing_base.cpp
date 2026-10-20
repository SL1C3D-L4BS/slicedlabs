// sl_runtime_testing (WO-076): helpers shared by the runtime tests land here in later work orders.
// This file keeps the library non-empty and names the version of the helper set.

namespace sl::runtime::testing {

/// The helper-set version; bumped when a shared helper changes behaviour.
int helpers_version() noexcept { return 1; }

} // namespace sl::runtime::testing
