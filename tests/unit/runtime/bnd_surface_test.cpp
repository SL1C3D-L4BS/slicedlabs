// RT-INV-BND-08 (SL-ARCH-003 ARCH3-R148, ADR-0027): the runtime's window on authoritative data is
// a document projection, nothing more. world_link and document_source have no member that hands
// out a Fabric snapshot, store, workspace, transaction or transaction manager: requires-
// expressions prove the accessor names do not exist and every public member's return type, with
// references, pointers, result<> and smart pointers peeled off, is none of those types.

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/component.hpp"

#include <doctest/doctest.h>

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

namespace link = sl::runtime::link;
namespace fab = sl::fabric;

template <class T> struct peel {
  using type = std::remove_cvref_t<std::remove_pointer_t<std::remove_cvref_t<T>>>;
};
template <class T> struct peel<sl::result<T>> : peel<T> {};
template <class T> struct peel<std::unique_ptr<T>> : peel<T> {};
template <class T> struct peel<std::shared_ptr<T>> : peel<T> {};
template <class T> struct peel<std::weak_ptr<T>> : peel<T> {};
template <class T> using peeled = peel<std::remove_cvref_t<T>>::type;

template <class T>
concept fabric_handle =
    std::same_as<peeled<T>, fab::snapshot> || std::same_as<peeled<T>, fab::store_backend> ||
    std::same_as<peeled<T>, fab::workspace> || std::same_as<peeled<T>, fab::transaction> ||
    std::same_as<peeled<T>, fab::transaction_manager>;

// The peeling itself works.
static_assert(fabric_handle<const fab::snapshot &>);
static_assert(fabric_handle<sl::result<std::unique_ptr<fab::workspace>>>);
static_assert(fabric_handle<fab::transaction_manager *>);
static_assert(fabric_handle<const fab::store_backend *const &>);
static_assert(!fabric_handle<const fab::document_projection &>);

// No accessor of these names, const or not.
template <class T>
concept has_snapshot = requires(T &t) { t.snapshot(); };
template <class T>
concept has_store = requires(T &t) { t.store(); };
template <class T>
concept has_workspace = requires(T &t) { t.workspace(); };
template <class T>
concept has_transaction = requires(T &t) { t.transaction(); };
template <class T>
concept has_manager = requires(T &t) { t.manager(); };
template <class T>
concept has_head = requires(T &t) { t.head(); };
template <class T>
concept has_begin = requires(T &t) { t.begin(); };
template <class T>
concept hands_out_fabric = has_snapshot<T> || has_store<T> || has_workspace<T> ||
                           has_transaction<T> || has_manager<T> || has_head<T> || has_begin<T>;

static_assert(!hands_out_fabric<link::world_link>);
static_assert(!hands_out_fabric<const link::world_link>);
static_assert(!hands_out_fabric<link::document_source>);
static_assert(!hands_out_fabric<const link::document_source>);

// Every public member of world_link.
using wl = link::world_link;
static_assert(
    !fabric_handle<decltype(wl::attach(
        std::declval<sl::runtime::ecs::world &>(), std::declval<sl::runtime::frame_scheduler &>(),
        std::declval<sl::runtime::ecs::system_scheduler &>(),
        std::declval<sl::runtime::ecs::command_queue &>(),
        std::declval<sl::runtime::mem::diagnostics &>(),
        std::declval<const link::binding_registry &>(), std::declval<link::proposal_sink &>(),
        std::declval<link::link_config>()))>);
static_assert(!fabric_handle<decltype(std::declval<wl &>().ingest(
                  std::declval<const link::document_source &>()))>);
static_assert(!fabric_handle<decltype(std::declval<wl &>().post_projection(
                  std::declval<fab::document_projection>()))>);
static_assert(!fabric_handle<decltype(std::declval<wl &>().observe_component(
                  std::declval<sl::runtime::ecs::component_id>()))>);
static_assert(!fabric_handle<decltype(std::declval<const wl &>().entities())>);
static_assert(!fabric_handle<decltype(std::declval<const wl &>().stats())>);
static_assert(!fabric_handle<decltype(std::declval<const wl &>().projections_held())>);
static_assert(!fabric_handle<decltype(std::declval<const wl &>().current_generation())>);
static_assert(!fabric_handle<decltype(std::declval<const wl &>().system_id())>);

// Every public member of document_source; its projection is the one window.
using ds = link::document_source;
static_assert(!fabric_handle<decltype(ds::make(std::declval<fab::document_projection>()))>);
static_assert(!fabric_handle<decltype(std::declval<const ds &>().load_plan())>);
static_assert(std::same_as<decltype(std::declval<const ds &>().projection()),
                           const fab::document_projection &>);
static_assert(!std::is_base_of_v<fab::snapshot, fab::document_projection>);

// The sink sees patches and origins only.
static_assert(
    std::same_as<decltype(std::declval<link::proposal_sink &>().submit(
                     std::declval<const fab::patch &>(), std::declval<const fab::tx_origin &>())),
                 sl::result<void>>);

} // namespace

TEST_CASE("RT-INV-BND-08: world_link and document_source expose no Fabric store handle") {
  // The checks are the static_asserts above; this case records that the translation unit built.
  CHECK(fabric_handle<fab::workspace &>);
  CHECK_FALSE(hands_out_fabric<link::world_link>);
  CHECK_FALSE(hands_out_fabric<link::document_source>);
}
