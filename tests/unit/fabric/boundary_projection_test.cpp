// FAB-INV-BND-02 (SL-ARCH-001 ARCH1-R60; master plan section 2.1): document_projection exposes no
// mutator. Requires-expressions prove it at compile time: every member function is const, the
// records come out as record views with const bodies, nothing reaches the snapshot, the store or
// the manager, and no setter-shaped name exists. The run-time cases show what the projection does
// offer: the domain-prefix filter, record views, ids in ascending order, document ids and
// membership, the revision and root digests as cache keys, and a view that lives as long as any
// copy of the projection.

#include "sl/fabric/boundary.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;
using sl::result;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

// ---- compile-time proof: no non-const access ----------------------------------------------

using P = document_projection;

template <class M> struct is_const_member : std::false_type {};
template <class R, class C, class... A>
struct is_const_member<R (C::*)(A...) const> : std::true_type {};
template <class R, class C, class... A>
struct is_const_member<R (C::*)(A...) const noexcept> : std::true_type {};

// Every non-static public member function of the projection is const-qualified.
static_assert(is_const_member<decltype(&P::get)>::value);
static_assert(is_const_member<decltype(&P::check_scope)>::value);
static_assert(is_const_member<decltype(&P::for_each)>::value);
static_assert(is_const_member<decltype(&P::documents)>::value);
static_assert(is_const_member<decltype(&P::members)>::value);
static_assert(is_const_member<decltype(&P::for_each_in_document)>::value);
static_assert(is_const_member<decltype(&P::document_of)>::value);
static_assert(is_const_member<decltype(&P::size)>::value);
static_assert(is_const_member<decltype(&P::domains)>::value);
static_assert(is_const_member<decltype(&P::root_digest)>::value);
static_assert(is_const_member<decltype(&P::revision_digest)>::value);
static_assert(is_const_member<decltype(&P::generation)>::value);
static_assert(is_const_member<decltype(&P::read)>::value);

// The only constructor path is the factory: no default construction, no construction from a
// snapshot, and no subclass can add one (final).
static_assert(std::is_final_v<P>);
static_assert(!std::is_default_constructible_v<P>);
static_assert(!std::is_constructible_v<P, const snapshot &>);
static_assert(
    std::is_same_v<decltype(&P::make), result<P> (*)(const snapshot &, const schema_registry &,
                                                     std::span<const std::string>)>);

// What a reader gets back is const all the way down.
static_assert(
    std::is_same_v<decltype(std::declval<const P &>().get(record_id{})), result<record_view>>);
static_assert(std::is_const_v<std::remove_pointer_t<decltype(record_view::body)>>);
static_assert(
    std::is_same_v<decltype(std::declval<const P &>().documents()), std::span<const record_id>>);
static_assert(
    std::is_same_v<decltype(std::declval<const P &>().domains()), std::span<const std::string>>);
static_assert(std::is_same_v<decltype(std::declval<const P &>().revision_digest()),
                             const std::optional<digest> &>);
static_assert(std::is_same_v<decltype(std::declval<const P &>().root_digest()), const digest &>);
static_assert(std::is_same_v<P::visitor, std::function<result<void>(const record_view &)>>);

// A writable view of any kind would be a mutator.
template <class T> constexpr bool body_assignable = requires(T &v) { *v.body = value{}; };
static_assert(!body_assignable<record_view>);
template <class T>
constexpr bool documents_assignable = requires(T &p) { p.documents()[0] = record_id{}; };
static_assert(!documents_assignable<P>);
template <class T>
constexpr bool domains_assignable = requires(T &p) { p.domains()[0] = std::string{}; };
static_assert(!domains_assignable<P>);

// No way to reach the snapshot, the store, the manager or the registries behind the projection.
#define SL_BND_ABSENT(NAME, ...)                                                                   \
  template <class T> constexpr bool has_##NAME = requires(T &t) { t.NAME(__VA_ARGS__); };          \
  static_assert(!has_##NAME<P>, #NAME " must not exist on document_projection")

SL_BND_ABSENT(snapshot);
SL_BND_ABSENT(store);
SL_BND_ABSENT(manager);
SL_BND_ABSENT(schemas);
SL_BND_ABSENT(base);
// And no setter-shaped member, whatever its argument list.
SL_BND_ABSENT(set, record_id{}, value{});
SL_BND_ABSENT(set_body, record_id{}, value{});
SL_BND_ABSENT(put, record_id{}, value{});
SL_BND_ABSENT(insert, record_id{}, value{});
SL_BND_ABSENT(emplace, record_id{}, value{});
SL_BND_ABSENT(erase, record_id{});
SL_BND_ABSENT(remove, record_id{});
SL_BND_ABSENT(clear);
SL_BND_ABSENT(assign, record_id{}, value{});
SL_BND_ABSENT(update, record_id{}, value{});
SL_BND_ABSENT(apply, patch{});
SL_BND_ABSENT(commit);
SL_BND_ABSENT(propose, record_id{}, value{});
SL_BND_ABSENT(mutable_body, record_id{});
SL_BND_ABSENT(get_mut, record_id{});
SL_BND_ABSENT(at);
#undef SL_BND_ABSENT

// A const projection offers every read; a non-const one offers nothing more.
template <class T>
constexpr bool reads = requires(T &p, const record_id &id, const P::visitor &fn) {
  p.get(id);
  p.check_scope(id);
  p.for_each(fn);
  p.documents();
  p.members(id);
  p.for_each_in_document(id, fn);
  p.document_of(id);
  p.size();
  p.domains();
  p.root_digest();
  p.revision_digest();
  p.generation();
  p.read(id);
};
static_assert(reads<const P>);
static_assert(reads<P>);

// ---- run-time fixture ---------------------------------------------------------------------

struct fixture {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  std::unique_ptr<workspace> ws;
  record_id hero = tg::entity_named("hero");
  record_id villain = tg::entity_named("villain");
  record_id level = tg::entity_named("level");
  record_id mesh = tg::asset_named("rock");

  fixture() {
    ws = must(workspace::create_with(
        store, must(tg::scene_options(std::make_shared<manual_clock>(22), 4))));
    must(tg::commit_ops(ws->manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                        {must(tg::create_entity("hero")), must(tg::create_entity("villain")),
                         must(tg::create_scene(level, "level", {hero, villain})),
                         must(tg::create_mesh("rock", 128))}));
  }

  [[nodiscard]] transaction_manager &manager() const { return ws->manager(); }

  [[nodiscard]] sl::result<document_projection>
  project(const std::vector<std::string> &domains) const {
    return document_projection::make(must(manager().head()), manager().schemas(), domains);
  }
};

[[nodiscard]] std::vector<record_id> ids_of(const document_projection &p) {
  std::vector<record_id> out;
  REQUIRE(p.for_each([&](const record_view &v) -> sl::result<void> {
             out.push_back(v.id);
             return {};
           }).has_value());
  return out;
}

std::vector<record_id> sorted(std::vector<record_id> v) {
  std::ranges::sort(v);
  return v;
}

} // namespace

TEST_CASE("FAB-INV-BND-02: no member of document_projection mutates, proven by requires "
          "expressions (see the static_asserts above)") {
  // The assertions are compile-time; this case keeps the invariant bound to a run-time test and
  // checks the last property that needs an object: a record view is a plain value.
  fixture f;
  const document_projection p = must(f.project({"content.scene"}));
  record_view v = must(p.get(f.hero));
  CHECK(v.id == f.hero);
  CHECK(v.body != nullptr);
  CHECK(v.provenance_inputs.empty());
  // Changing the view changes the view, not the projection.
  v.body = nullptr;
  CHECK(must(p.get(f.hero)).body != nullptr);
}

TEST_CASE("FAB-INV-BND-02: the projection filters by domain prefix and shows records only") {
  fixture f;
  const auto scene = must(f.project({"content.scene"}));
  CHECK(scene.size() == 3);
  CHECK(ids_of(scene) == sorted({f.hero, f.villain, f.level}));
  const auto mesh = must(f.project({"content.mesh"}));
  CHECK(ids_of(mesh) == std::vector<record_id>{f.mesh});
  const auto content = must(f.project({"content"}));
  CHECK(ids_of(content) == sorted({f.hero, f.villain, f.level, f.mesh}));
  // A prefix is dotted: "content.sc" is a domain of its own and holds nothing.
  CHECK(must(f.project({"content.sc"})).size() == 0);
  CHECK(must(f.project({"asset"})).size() == 0);
  // Several domains, repeated ones collapsed, listed sorted.
  const auto both = must(f.project({"content.mesh", "content.scene", "content.mesh"}));
  CHECK(both.size() == 4);
  CHECK(std::vector<std::string>(both.domains().begin(), both.domains().end()) ==
        std::vector<std::string>{"content.mesh", "content.scene"});
  // The views carry the stored record.
  const record_view v = must(content.get(f.mesh));
  const snapshot_entry stored = must(must(f.manager().head()).get(f.mesh));
  CHECK(v.id == stored.id);
  CHECK(v.type == stored.type);
  REQUIRE(v.body != nullptr);
  CHECK(*v.body == stored.body);
}

TEST_CASE("FAB-INV-BND-02: what is outside, absent or retired is refused, never shown") {
  fixture f;
  const auto scene = must(f.project({"content.scene"}));
  const auto outside = scene.get(f.mesh);
  REQUIRE_FALSE(outside.has_value());
  CHECK(outside.error().code == errc::authority_denied);
  CHECK(scene.check_scope(f.mesh).error().code == errc::authority_denied);
  CHECK(scene.document_of(f.mesh).error().code == errc::authority_denied);
  const auto ghost = scene.get(tg::entity_named("ghost"));
  REQUIRE_FALSE(ghost.has_value());
  CHECK(ghost.error().code == errc::not_found);
  CHECK(scene.check_scope(f.hero).has_value());
  // As a state_reader (what a proposal is normalised over): absent is nullopt, outside is denied.
  CHECK(must(scene.read(f.hero)).has_value());
  CHECK_FALSE(must(scene.read(tg::entity_named("ghost"))).has_value());
  CHECK(scene.read(f.mesh).error().code == errc::authority_denied);

  // A retired record leaves the next projection and reads as not_found.
  must(tg::commit_ops(f.manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                      {op_delete(f.mesh)}));
  const auto content = must(f.project({"content"}));
  CHECK(content.size() == 3);
  CHECK(content.get(f.mesh).error().code == errc::not_found);
}

TEST_CASE("FAB-INV-BND-02: bad domain lists and unknown types are refused by make") {
  fixture f;
  const snapshot head = must(f.manager().head());
  for (const std::vector<std::string> &bad :
       {std::vector<std::string>{}, std::vector<std::string>{""},
        std::vector<std::string>{"Content"}, std::vector<std::string>{"nonsense"},
        std::vector<std::string>{"content.scene", "content..mesh"}}) {
    const auto r = document_projection::make(head, f.manager().schemas(), bad);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::invalid_argument);
  }
  // A registry that does not know the record types cannot judge their domains.
  schema_registry empty;
  REQUIRE(empty.freeze().has_value());
  const auto r = document_projection::make(head, empty, std::vector<std::string>{"content"});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::schema_unknown);
}

TEST_CASE("FAB-INV-BND-02: iteration is ordered, stops at the first error and returns it "
          "unchanged") {
  fixture f;
  const auto content = must(f.project({"content"}));
  const std::vector<record_id> all = ids_of(content);
  CHECK(std::ranges::is_sorted(all));
  CHECK(all.size() == 4);

  std::size_t seen = 0;
  const auto stopped = content.for_each([&](const record_view &) -> sl::result<void> {
    ++seen;
    if (seen == 2) {
      return sl::make_error(errc::conflict, "stop here");
    }
    return {};
  });
  REQUIRE_FALSE(stopped.has_value());
  CHECK(stopped.error().code == errc::conflict);
  CHECK(stopped.error().message == "stop here");
  CHECK(seen == 2);
}

TEST_CASE("FAB-INV-BND-02: document ids and membership are readable") {
  fixture f;
  const auto content = must(f.project({"content"}));
  // A document root belongs to its own document; the entities, created before it existed, to none.
  CHECK(std::vector<record_id>(content.documents().begin(), content.documents().end()) ==
        sorted({f.level, f.mesh}));
  CHECK(content.members(f.level) == std::vector<record_id>{f.level});
  CHECK(content.members(f.mesh) == std::vector<record_id>{f.mesh});
  CHECK(content.members(f.hero).empty());
  CHECK(content.members(tg::entity_named("ghost")).empty());
  CHECK(must(content.document_of(f.level)) == f.level);
  CHECK(must(content.document_of(f.hero)).is_null());

  // The projection's membership is the snapshot's, cut to its domains.
  const snapshot head = must(f.manager().head());
  CHECK(content.members(f.level) == must(head.members(f.level)));
  const auto scene = must(f.project({"content.scene"}));
  CHECK(std::vector<record_id>(scene.documents().begin(), scene.documents().end()) ==
        std::vector<record_id>{f.level});

  std::vector<record_id> walked;
  REQUIRE(content
              .for_each_in_document(f.mesh,
                                    [&](const record_view &v) -> sl::result<void> {
                                      walked.push_back(v.id);
                                      return {};
                                    })
              .has_value());
  CHECK(walked == std::vector<record_id>{f.mesh});
}

TEST_CASE("FAB-INV-BND-02: the revision and root digests key caches") {
  fixture f;
  const snapshot head = must(f.manager().head());
  const auto a = must(f.project({"content.scene"}));
  const auto b = must(f.project({"content"}));
  CHECK(a.revision_digest() == head.revision_digest());
  CHECK(a.root_digest() == head.root_digest());
  CHECK(a.generation() == head.generation());
  // The digests name the document state, not the filter.
  CHECK(a.root_digest() == b.root_digest());
  CHECK(a.revision_digest() == b.revision_digest());

  const commit_receipt r =
      must(tg::commit_ops(f.manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                          {must(tg::set_name(f.hero, "hero 2"))}));
  const auto c = must(f.project({"content.scene"}));
  CHECK(c.revision_digest() == r.revision);
  CHECK(c.root_digest() != a.root_digest());
  CHECK(c.generation() == a.generation() + 1);
  // The old projection keeps describing the old state.
  CHECK(a.revision_digest() == head.revision_digest());
  CHECK(*must(a.get(f.hero)).body != *must(c.get(f.hero)).body);
}

TEST_CASE("FAB-INV-BND-02: a view lives as long as any copy of the projection, and any number of "
          "threads may read one") {
  fixture f;
  auto original = std::make_unique<document_projection>(must(f.project({"content"})));
  const document_projection copy = *original;
  const record_view v = must(original->get(f.hero));
  original.reset();
  const record_view w = must(copy.get(f.hero));
  CHECK(w.body == v.body); // the copies share the data
  CHECK(w.body->get({path_segment::field(1)})->as_string() == "hero");

  std::atomic<int> ok{0};
  std::atomic<std::size_t> visited{0};
  // One visitor shared by the readers, built outside them, so no reader owns a std::function
  // across for_each (gcc -fanalyzer, WO-064).
  const document_projection::visitor count = [&visited](const record_view &) -> sl::result<void> {
    visited.fetch_add(1, std::memory_order_relaxed);
    return {};
  };
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      for (int round = 0; round < 50; ++round) {
        // Two statements, not one condition with a conditional temporary (gcc -fanalyzer).
        const bool walked = copy.for_each(count).has_value();
        const bool found = copy.get(f.mesh).has_value();
        if (!walked || !found) {
          return;
        }
      }
      ++ok;
    });
  }
  for (std::thread &t : readers) {
    t.join();
  }
  CHECK(ok.load() == 4);
  CHECK(visited.load() == 4U * 200U); // four records, 50 rounds, four readers
}
