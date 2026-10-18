// FAB-INV-STO-07 (SL-ARCH-002 ARCH2-R01, ADR-0016): mutation_capability is not constructible,
// copyable or movable outside its friends, and store_backend::commit cannot be called without one.
// The concepts below are checked in this (non-friend) scope, so access control applies; each
// static_assert would fail to hold if the passkey were weakened. memory_backend has no other
// mutator: put, ref writes and intent writes exist only inside commit.

#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sl::fabric::commit_ack;
using sl::fabric::commit_batch;
using sl::fabric::memory_backend;
using sl::fabric::mutation_capability;
using sl::fabric::object;
using sl::fabric::store_backend;

template <class C>
concept default_constructible_here = requires { C{}; };
template <class C>
concept paren_constructible_here = requires { C(); };
template <class C>
concept copy_constructible_here = requires(const C &c) { C{c}; };
template <class C>
concept move_constructible_here = requires(C &c) { C{std::move(c)}; };
template <class C>
concept copy_assignable_here = requires(C &l, const C &r) { l = r; };
template <class C>
concept move_assignable_here = requires(C &l, C &r) { l = std::move(r); };
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
template <class S>
concept has_put = requires(S &s, const object &o) { s.put(o); };
template <class S>
concept has_write_ref = requires(S &s, const sl::fabric::digest &d) { s.write_ref("main", d); };
template <class S>
concept has_set_ref = requires(S &s, const sl::fabric::digest &d) { s.set_ref("main", d); };

// No construction, copy or move outside the friends.
static_assert(!default_constructible_here<mutation_capability>);
static_assert(!paren_constructible_here<mutation_capability>);
static_assert(!copy_constructible_here<mutation_capability>);
static_assert(!move_constructible_here<mutation_capability>);
static_assert(!copy_assignable_here<mutation_capability>);
static_assert(!move_assignable_here<mutation_capability>);
static_assert(!std::is_default_constructible_v<mutation_capability>);
static_assert(!std::is_copy_constructible_v<mutation_capability>);
static_assert(!std::is_move_constructible_v<mutation_capability>);
static_assert(!std::is_copy_assignable_v<mutation_capability>);
static_assert(!std::is_move_assignable_v<mutation_capability>);
static_assert(!std::is_aggregate_v<mutation_capability>);

// Commit is uncallable without a capability, on the interface and on the memory backend.
static_assert(!commit_without_capability<store_backend>);
static_assert(!commit_with_braced_capability<store_backend>);
static_assert(!commit_with_null_capability<store_backend>);
static_assert(!commit_without_capability<memory_backend>);
static_assert(!commit_with_braced_capability<memory_backend>);
static_assert(commit_with_capability<store_backend>);
static_assert(commit_with_capability<memory_backend>);
static_assert(std::is_same_v<decltype(&store_backend::commit),
                             sl::result<commit_ack> (store_backend::*)(const mutation_capability &,
                                                                       commit_batch) noexcept>);

// No mutator other than commit.
static_assert(!has_put<memory_backend> && !has_put<store_backend>);
static_assert(!has_write_ref<memory_backend> && !has_set_ref<memory_backend>);
static_assert(!std::is_copy_constructible_v<memory_backend>);
static_assert(!std::is_move_constructible_v<memory_backend>);

} // namespace

TEST_CASE("FAB-INV-STO-07: the capability passkey holds (static checks above compiled)") {
  CHECK_FALSE(std::is_default_constructible_v<mutation_capability>);
  CHECK_FALSE(commit_without_capability<store_backend>);
}

TEST_CASE("FAB-INV-STO-07: the test-only backdoor is the one capability holder in tests") {
  memory_backend store;
  const std::vector<std::uint8_t> bytes = {'x'};
  const object o = sl::fabric::make_object(sl::fabric::object_kind::blob, bytes);
  commit_batch batch;
  batch.objects.push_back(o);
  const sl::result<commit_ack> ack = sl::fabric::testing::backdoor::commit(store, std::move(batch));
  REQUIRE(ack.has_value());
  CHECK(ack->seq == 1);
  CHECK(store.has(o.id).value_or(false));
}
