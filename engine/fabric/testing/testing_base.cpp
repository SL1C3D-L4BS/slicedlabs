// sl_fabric_testing (WO-037): helpers shared by the fabric tests land here in later work orders.
// This file keeps the library non-empty and names the version of the helper set.

namespace sl::fabric::testing {

/// The helper-set version; bumped when a shared helper changes behaviour.
int helpers_version() noexcept { return 1; }

} // namespace sl::fabric::testing
