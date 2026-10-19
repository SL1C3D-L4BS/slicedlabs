#pragma once

// The F5 mutation ledger (WO-051, SL-ARCH-002 ARCH2-R01 and ARCH2-R17, ADR-0016; SL-GOV-006 F5:
// mutations bypassing the transaction manager over all mutations, instrumented in debug builds,
// threshold 0).
//
// F5 is carried by structure: store_backend::commit needs a mutation_capability, which only
// transaction_manager and the test-only testing::backdoor can make (store.hpp). This ledger is the
// detector behind that barrier, compiled only when SL_FABRIC_F5_INSTRUMENT is set (Debug and every
// sanitizer build, engine/fabric/CMakeLists.txt); in other builds every function here is an
// inline no-op and nothing is counted.
//
//   - The manager opens an authorized_scope around its backend commit (step C4). The scope takes
//     the capability, so only a capability holder can open one; its depth is per thread.
//   - Every mutating site of a backend calls note_mutation(site, store): `mutations` counts every
//     call, and a call with no authorized_scope open on the thread also counts in `bypassing` and
//     logs an event of system "fabric/f5" naming the site and the counters.
//   - On a governed store (a live transaction manager holds a governed_store for it), or when no
//     store is named, the bypass is real: the event is fatal and SL_DEBUG_ASSERT trips. A commit
//     on a store no manager governs can only come from the test backdoor (production code cannot
//     make a capability), so it is counted and logged at debug level, below the default, and not
//     asserted: tests seed bare stores that way without printing a fatal line (WO-064).
//
// The site must be a string literal (static storage): last_bypass_site() returns the view.

#include <cstdint>
#include <string_view>

#include "sl/fabric/store.hpp"

namespace sl::fabric::f5 {

/// True when the ledger is compiled in (SL_FABRIC_F5_INSTRUMENT).
#if defined(SL_FABRIC_F5_INSTRUMENT) && SL_FABRIC_F5_INSTRUMENT
inline constexpr bool enabled = true;
#else
inline constexpr bool enabled = false;
#endif

/// The log system of a bypass event.
inline constexpr std::string_view log_system = "fabric/f5";

/// The process-wide counters: the F5 denominator (`mutations`) and numerator (`bypassing`).
struct ledger {
  std::uint64_t mutations = 0;
  std::uint64_t bypassing = 0;

  friend bool operator==(const ledger &, const ledger &) noexcept = default;
};

namespace detail {
// Defined in src/f5.cpp; called only when `enabled`.
[[nodiscard]] ledger read() noexcept;
void reset() noexcept;
[[nodiscard]] std::string_view last_bypass_site() noexcept;
void enter() noexcept;
void leave() noexcept;
void note(std::string_view site, const store_backend *store) noexcept;
void govern(const store_backend *store) noexcept;
void release(const store_backend *store) noexcept;
} // namespace detail

/// The counters so far; {0, 0} when the ledger is compiled out.
[[nodiscard]] inline ledger read() noexcept {
  if constexpr (enabled) {
    return detail::read();
  } else {
    return {};
  }
}

/// Zeroes the counters and forgets the last bypass site (tests only).
inline void reset_for_test() noexcept {
  if constexpr (enabled) {
    detail::reset();
  }
}

/// The site of the most recent bypass; empty when there was none (or the ledger is compiled out).
[[nodiscard]] inline std::string_view last_bypass_site() noexcept {
  if constexpr (enabled) {
    return detail::last_bypass_site();
  } else {
    return {};
  }
}

/// Counts one backend mutation at `site` (a string literal) of `store` (null: no store named).
/// Outside an authorized_scope it is a bypass: counted and logged as a "fabric/f5" event, fatal
/// with SL_DEBUG_ASSERT for a governed or unnamed store, debug for an ungoverned one.
inline void note_mutation(std::string_view site, const store_backend *store = nullptr) noexcept {
  if constexpr (enabled) {
    detail::note(site, store);
  } else {
    static_cast<void>(site);
    static_cast<void>(store);
  }
}

/// RAII: the calling thread is inside the transaction manager's commit for the scope's lifetime.
/// Scopes nest. Only a holder of a mutation_capability can open one.
class authorized_scope {
public:
  explicit authorized_scope(const mutation_capability & /*cap*/) noexcept {
    if constexpr (enabled) {
      detail::enter();
    }
  }
  authorized_scope(const authorized_scope &) = delete;
  authorized_scope(authorized_scope &&) = delete;
  authorized_scope &operator=(const authorized_scope &) = delete;
  authorized_scope &operator=(authorized_scope &&) = delete;
  ~authorized_scope() {
    if constexpr (enabled) {
      detail::leave();
    }
  }
};

/// RAII: `store` is governed by a transaction manager for the object's lifetime (counted, so two
/// managers over one store keep it governed until both are gone).
class governed_store {
public:
  explicit governed_store(const store_backend *store) noexcept : store_(store) {
    if constexpr (enabled) {
      detail::govern(store_);
    }
  }
  governed_store(const governed_store &) = delete;
  governed_store(governed_store &&) = delete;
  governed_store &operator=(const governed_store &) = delete;
  governed_store &operator=(governed_store &&) = delete;
  ~governed_store() {
    if constexpr (enabled) {
      detail::release(store_);
    }
  }

private:
  const store_backend *store_;
};

} // namespace sl::fabric::f5
