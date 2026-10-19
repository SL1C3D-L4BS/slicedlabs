// The F5 mutation ledger (WO-051, SL-ARCH-002 ARCH2-R17, ADR-0016). See sl/fabric/f5.hpp. These
// functions are reached only from the inline wrappers when SL_FABRIC_F5_INSTRUMENT is set; in
// other builds nothing references this file and the linker leaves it out.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric::f5::detail {

namespace {

struct ledger_state {
  std::atomic<std::uint64_t> mutations{0};
  std::atomic<std::uint64_t> bypassing{0};
  std::mutex mutex; // guards the two members below
  std::string_view last_site;
  std::map<const store_backend *, std::uint64_t, std::less<>> governed; // store -> managers
};

ledger_state &state() noexcept {
  static ledger_state s;
  return s;
}

int &depth() noexcept {
  thread_local int d = 0;
  return d;
}

} // namespace

ledger read() noexcept {
  const ledger_state &s = state();
  return ledger{.mutations = s.mutations.load(), .bypassing = s.bypassing.load()};
}

void reset() noexcept {
  ledger_state &s = state();
  const std::scoped_lock lock(s.mutex);
  s.mutations.store(0);
  s.bypassing.store(0);
  s.last_site = {};
}

std::string_view last_bypass_site() noexcept {
  ledger_state &s = state();
  const std::scoped_lock lock(s.mutex);
  return s.last_site;
}

void enter() noexcept { ++depth(); }

void leave() noexcept { --depth(); }

void govern(const store_backend *store) noexcept {
  if (store == nullptr) {
    return;
  }
  ledger_state &s = state();
  const std::scoped_lock lock(s.mutex);
  ++s.governed[store];
}

void release(const store_backend *store) noexcept {
  if (store == nullptr) {
    return;
  }
  ledger_state &s = state();
  const std::scoped_lock lock(s.mutex);
  const auto it = s.governed.find(store);
  if (it != s.governed.end() && --it->second == 0) {
    s.governed.erase(it);
  }
}

void note(std::string_view site, const store_backend *store) noexcept {
  ledger_state &s = state();
  const std::uint64_t mutations = s.mutations.fetch_add(1) + 1;
  if (depth() > 0) {
    return;
  }
  const std::uint64_t bypassing = s.bypassing.fetch_add(1) + 1;
  bool governed = store == nullptr;
  {
    const std::scoped_lock lock(s.mutex);
    s.last_site = site;
    governed = governed || s.governed.contains(store);
  }
  // A store no transaction manager governs is a test store seeded through testing::backdoor:
  // counted, logged at debug (below the default level, so a passing test prints no line). A
  // governed or unnamed store is a real bypass: fatal, then the debug assertion.
  sl::log::emit(
      governed ? sl::log::level::fatal : sl::log::level::debug, log_system,
      "backend mutation outside f5::authorized_scope: it bypassed the transaction manager",
      {{"site", site}, {"governed", governed}, {"mutations", mutations}, {"bypassing", bypassing}});
  SL_DEBUG_ASSERT(!governed, "F5: backend mutation at " + std::string(site) +
                                 " bypassed the transaction manager (ADR-0016)");
}

} // namespace sl::fabric::f5::detail
