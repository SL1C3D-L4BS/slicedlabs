// FAB-INV-F5-04 (SL-ARCH-002 ARCH2-R01, ADR-0016): commit without a capability and capability
// construction outside the friends do not compile. Requires-expressions under static_assert,
// checked here in a non-friend scope so access control applies; each static_assert would stop
// holding if the barrier were weakened. This extends the checks of store_capability_test
// (FAB-INV-STO-07: the capability's special members, memory_backend::commit) and tx_history_test
// (the manager's private write_batch) to the rest of the surface: log_backend, the F5 ledger's
// scope, derivation from the capability, the const store views of workspace, manager and
// snapshot, the backdoor, and the read-only value and snapshot APIs.

#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using namespace sl::fabric;

// ---- commit needs the capability on every backend ----

template <class S>
concept commit_without_capability = requires(S &s, commit_batch b) { s.commit(std::move(b)); };
template <class S>
concept commit_with_braced_capability =
    requires(S &s, commit_batch b) { s.commit({}, std::move(b)); };
template <class S>
concept commit_with_null_capability =
    requires(S &s, commit_batch b) { s.commit(nullptr, std::move(b)); };
template <class S>
concept commit_with_capability = requires(S &s, const mutation_capability &cap, commit_batch b) {
  { s.commit(cap, std::move(b)) } noexcept -> std::same_as<sl::result<commit_ack>>;
};
static_assert(!commit_without_capability<log_backend>);
static_assert(!commit_with_braced_capability<log_backend>);
static_assert(!commit_with_null_capability<log_backend>);
static_assert(commit_with_capability<log_backend>);
static_assert(!commit_with_null_capability<memory_backend>);

// ---- the capability cannot be forged ----

// Derivation does not reach the private constructor.
struct thief : mutation_capability {};
template <class C>
concept paren_constructible_here = requires { C(); };
static_assert(!std::is_default_constructible_v<thief>);
static_assert(!paren_constructible_here<thief>);
static_assert(!std::is_constructible_v<mutation_capability, thief>);
// No conversion from anything at hand.
static_assert(!std::is_constructible_v<mutation_capability, int>);
static_assert(!std::is_constructible_v<mutation_capability, std::nullptr_t>);
static_assert(!std::is_constructible_v<mutation_capability, commit_batch>);
static_assert(!std::is_convertible_v<commit_batch, const mutation_capability &>);
// The backdoor is not an object and cannot be made.
static_assert(!std::is_default_constructible_v<testing::backdoor>);
static_assert(std::is_same_v<decltype(&testing::backdoor::commit),
                             sl::result<commit_ack> (*)(store_backend &, commit_batch) noexcept>);

// ---- the F5 scope is reachable only with a capability ----

template <class C>
concept scope_from_nothing = requires { C{}; };
static_assert(!std::is_default_constructible_v<f5::authorized_scope>);
static_assert(!scope_from_nothing<f5::authorized_scope>);
static_assert(std::is_nothrow_constructible_v<f5::authorized_scope, const mutation_capability &>);
static_assert(!std::is_convertible_v<const mutation_capability &, f5::authorized_scope>);
static_assert(!std::is_constructible_v<f5::authorized_scope, std::nullptr_t>);
static_assert(!std::is_copy_constructible_v<f5::authorized_scope>);
static_assert(!std::is_move_constructible_v<f5::authorized_scope>);
static_assert(!std::is_copy_constructible_v<f5::governed_store>);
static_assert(!std::is_move_constructible_v<f5::governed_store>);
static_assert(noexcept(f5::read()));
static_assert(noexcept(f5::note_mutation(std::string_view{})));
static_assert(noexcept(f5::last_bypass_site()));

// ---- every store view handed out is const ----

template <class W>
concept backdoor_through_workspace =
    requires(W &w, commit_batch b) { testing::backdoor::commit(w.store(), std::move(b)); };
template <class M>
concept backdoor_through_manager =
    requires(M &m, commit_batch b) { testing::backdoor::commit(m.store(), std::move(b)); };
template <class S>
concept backdoor_through_snapshot =
    requires(S &s, commit_batch b) { testing::backdoor::commit(s.store(), std::move(b)); };
template <class S>
concept backdoor_on_const_store =
    requires(const S &s, commit_batch b) { testing::backdoor::commit(s, std::move(b)); };
static_assert(!backdoor_through_workspace<workspace>);
static_assert(!backdoor_through_manager<transaction_manager>);
static_assert(!backdoor_through_snapshot<snapshot>);
static_assert(!backdoor_on_const_store<store_backend>);
static_assert(std::is_same_v<decltype(std::declval<workspace &>().store()), const store_backend &>);
static_assert(
    std::is_same_v<decltype(std::declval<transaction_manager &>().store()), const store_backend &>);
static_assert(std::is_same_v<decltype(std::declval<snapshot &>().store()), const store_backend &>);

// ---- values and snapshots are immutable ----

template <class V>
concept value_has_setter = requires(V &v, const path &p, V x) { v.set(p, std::move(x)); };
template <class V>
concept value_assigns_in_place = requires(V &v) { v.payload() = std::declval<const V &>(); };
template <class V>
concept value_edits_are_copies = requires(const V &v, const path &p, V x) {
  { v.with(p, std::move(x)) } -> std::same_as<sl::result<V>>;
  { v.without(p) } -> std::same_as<sl::result<V>>;
};
template <class S>
concept snapshot_has_put = requires(S &s, const record_id &id, value v) { s.put(id, v); };
template <class S>
concept snapshot_has_set_root = requires(S &s, const digest &d) { s.set_root(d); };
template <class S>
concept snapshot_root_assignable = requires(S &s, const digest &d) { s.root_digest() = d; };
static_assert(!value_has_setter<value>);
static_assert(!value_assigns_in_place<value>);
static_assert(value_edits_are_copies<value>);
static_assert(std::is_same_v<decltype(std::declval<value &>().payload()), const value &>);
static_assert(std::is_same_v<decltype(std::declval<value &>().items()), std::span<const value>>);
static_assert(std::is_same_v<decltype(std::declval<value &>().fields()), std::span<const field>>);
static_assert(!snapshot_has_put<snapshot>);
static_assert(!snapshot_has_set_root<snapshot>);
static_assert(!snapshot_root_assignable<snapshot>);

} // namespace

TEST_CASE("FAB-INV-F5-04: commit without a capability and forged capabilities do not compile") {
  // The static_asserts above are the test; these restate two of them at run time.
  CHECK_FALSE(commit_without_capability<log_backend>);
  CHECK_FALSE(std::is_default_constructible_v<thief>);
  CHECK_FALSE(scope_from_nothing<f5::authorized_scope>);
  CHECK_FALSE(backdoor_through_workspace<workspace>);
  CHECK_FALSE(value_has_setter<value>);
}
