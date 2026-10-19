// FAB-INV-REF-03 (SL-ARCH-001 ARCH1-R63): resolution is a pure function of the snapshot and the
// pins. Seeded projects (records in several documents, some retired) with two pinned packages
// sharing ids, and seeded references (ids live, retired, absent, in packages; random accepted
// types, referring documents, package qualifiers and external references): resolving the list
// again, in a shuffled order, through a second resolver over a freshly opened snapshot of the same
// revision and a manifest that differs in everything but the pins (vector order reversed), gives
// the same resolutions; nothing is written to the store, and the provider is asked for the pinned
// revisions only.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/resolver.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value body(std::uint64_t n) { return must(value::struct_({field{.no = 1, .v = value::uint(n)}})); }
type_id rec(std::uint64_t i) { return type_id_of(std::format("gen.rec{}", i)); }
record_id named(id_class c, std::string_view name) {
  return derive(c, well_known(id_class::namespace_, 1), name);
}
record_id pkg(std::string_view name) { return named(id_class::package, name); }

struct world {
  schema_registry reg;
  memory_backend store;
  std::uint32_t tx = 1;

  world() {
    REQUIRE(tg::add_tree_record_types(reg).has_value());
    REQUIRE(gen::fabric_identity::register_types(reg).has_value());
  }
  [[nodiscard]] snapshot empty() const { return must(snapshot::empty(store)); }
  snapshot build(const snapshot &base, const std::function<void(revision_builder &)> &edit) {
    revision_builder b(base, reg);
    edit(b);
    const built_revision built = must(b.build(tg::tree_inputs("resolver", tx++)));
    REQUIRE(tg::commit_built(store, built).has_value());
    return must(snapshot::open(store, built.revision));
  }
};

class map_provider final : public package_provider {
public:
  std::map<record_id, const store_backend *> stores;
  std::set<std::pair<record_id, digest>> asked;

  sl::result<snapshot> open(const record_id &package, const digest &revision) override {
    asked.emplace(package, revision);
    const auto it = stores.find(package);
    if (it == stores.end()) {
      return sl::make_error(errc::not_found, "package " + package.to_string() + " not mounted");
    }
    return snapshot::open(*it->second, revision);
  }
};

gen::oset_item<package_pin> pin_item(const record_id &package, const snapshot &s,
                                     std::uint8_t key) {
  return {.element = named(id_class::element, package.to_string()),
          .order_key = {key},
          .v = package_pin{.package = package,
                           .revision = s.revision_digest().value_or(digest{}),
                           .mount = "lib/" + package.to_string()}};
}

digest digest_of(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

// A seeded record of the pool, written with a random type, body and document (or none).
void put_random(revision_builder &b, tg::rng &r, const record_id &id,
                const std::vector<record_id> &docs) {
  const record_id doc = r.chance(30) ? record_id{} : docs[r.below(docs.size())];
  REQUIRE(b.put(id, record_write{.type = rec(r.below(3)),
                                 .document = doc,
                                 .body = body(r.below(1000)),
                                 .version = std::nullopt})
              .has_value());
}

// Every outcome as text, for a readable failure.
std::string show(const sl::result<resolution> &r) {
  if (!r) {
    return std::string("error ") + std::string(sl::to_string(r.error().code));
  }
  return std::format("{} {} {}", to_string(r->outcome), to_string(r->scope),
                     r->package ? r->package->to_string() : std::string("-"));
}

bool same(const sl::result<resolution> &a, const sl::result<resolution> &b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return a ? *a == *b : a.error().code == b.error().code;
}

} // namespace

TEST_CASE("FAB-INV-REF-03: resolution is a pure function of the snapshot and the pins") {
  const std::size_t seeds = 6;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    tg::rng r(0x2EF0'0000ULL + seed);
    world w;
    const std::vector<record_id> docs = {well_known(id_class::entity, 100),
                                         well_known(id_class::entity, 101),
                                         well_known(id_class::entity, 102)};
    const std::vector<record_id> pool = tg::tree_ids(60, id_class::entity, "ref");
    const std::vector<std::string> files = {"a.glb", "b.glb", "c.glb", "d.glb"};

    // The project: two thirds of the pool, some retired; import records for three files.
    std::vector<record_id> retired;
    snapshot project = w.build(w.empty(), [&](revision_builder &b) {
      for (const record_id &id : pool) {
        if (r.chance(66)) {
          put_random(b, r, id, docs);
          if (r.chance(20)) {
            retired.push_back(id);
          }
        }
      }
      for (std::size_t f = 0; f + 1 < files.size(); ++f) {
        const import_record ir = must(
            make_import_record(id_class::entity, "file", files[f], files[f], digest_of(files[f])));
        REQUIRE(b.put(import_record_id_for("file", files[f]),
                      record_write{.type = type_id_of("fabric.identity.import_record"),
                                   .document = record_id{},
                                   .body = must(ir.to_value()),
                                   .version = std::nullopt})
                    .has_value());
        if (r.chance(70)) {
          put_random(b, r, ir.record, docs);
        }
      }
    });
    if (!retired.empty()) {
      project = w.build(project, [&](revision_builder &b) {
        for (const record_id &id : retired) {
          REQUIRE(b.retire(id).has_value());
        }
      });
    }
    // Two packages over overlapping halves of the pool, one with a retired id.
    const record_id p1 = pkg("pkg/one");
    const record_id p2 = pkg("pkg/two");
    snapshot pkg1 = w.build(w.empty(), [&](revision_builder &b) {
      for (std::size_t i = 0; i < (pool.size() / 2) + 10; ++i) {
        put_random(b, r, pool[i], docs);
      }
    });
    pkg1 = w.build(pkg1, [&](revision_builder &b) { REQUIRE(b.retire(pool[3]).has_value()); });
    const snapshot pkg2 = w.build(w.empty(), [&](revision_builder &b) {
      for (std::size_t i = (pool.size() / 2) - 10; i < pool.size(); ++i) {
        put_random(b, r, pool[i], docs);
      }
    });
    map_provider provider;
    provider.stores = {{p1, &w.store}, {p2, &w.store}};
    project_manifest m;
    m.name = "one";
    m.package_pins = {pin_item(p1, pkg1, 0x40), pin_item(p2, pkg2, 0x60)};

    // Seeded references with their referring documents.
    std::vector<std::pair<reference, record_id>> refs;
    for (std::size_t k = 0; k < 120; ++k) {
      reference ref;
      if (r.chance(15)) {
        const std::string &file = files[r.below(files.size())];
        ref.external = external_ref{.system = "file", .locator = file, .content = std::nullopt};
      } else {
        ref.target = r.chance(90) ? pool[r.below(pool.size())] : named(id_class::entity, "absent");
      }
      if (r.chance(20)) {
        const record_id pinned = r.chance(50) ? p1 : p2;
        ref.package = r.chance(80) ? pinned : pkg("pkg/unpinned");
      }
      for (std::uint64_t t = 0; t < 3; ++t) {
        if (r.chance(25)) {
          ref.expected.push_back(rec(t));
        }
      }
      const record_id from = r.chance(30) ? record_id{} : docs[r.below(docs.size())];
      refs.emplace_back(std::move(ref), from);
    }

    const std::size_t objects = w.store.object_count();
    const std::uint64_t seq = w.store.last_seq();
    const resolver first(project, m, provider);
    std::vector<sl::result<resolution>> baseline;
    std::set<resolution_outcome> seen;
    for (const auto &[ref, from] : refs) {
      baseline.push_back(first.resolve(ref, from));
      REQUIRE_MESSAGE(baseline.back().has_value(), show(baseline.back()));
      seen.insert(baseline.back()->outcome);
    }
    CHECK(seen.size() >= 4); // the seeds exercise most outcomes

    // Again: the same answers.
    for (std::size_t k = 0; k < refs.size(); ++k) {
      const auto again = first.resolve(refs[k].first, refs[k].second);
      CHECK_MESSAGE(same(again, baseline[k]), (show(again) + " vs " + show(baseline[k])));
    }
    // Shuffled order: each reference still gets its answer.
    std::vector<std::size_t> order(refs.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
      order[k] = k;
    }
    for (std::size_t k = order.size(); k > 1; --k) {
      std::swap(order[k - 1], order[r.below(k)]);
    }
    for (const std::size_t k : order) {
      const auto again = first.resolve(refs[k].first, refs[k].second);
      CHECK_MESSAGE(same(again, baseline[k]), (show(again) + " vs " + show(baseline[k])));
    }
    // A second resolver over a fresh snapshot of the same revision and a manifest equal only in
    // its pins (vector order reversed: pin order is the oset order).
    project_manifest other = m;
    other.name = "two";
    other.default_branch = "elsewhere";
    std::ranges::reverse(other.package_pins);
    const snapshot reopened =
        must(snapshot::open(w.store, project.revision_digest().value_or(digest{})));
    map_provider provider2;
    provider2.stores = provider.stores;
    const resolver second(reopened, other, provider2);
    for (const std::size_t k : order) {
      const auto again = second.resolve(refs[k].first, refs[k].second);
      CHECK_MESSAGE(same(again, baseline[k]), (show(again) + " vs " + show(baseline[k])));
    }

    // Nothing written; only the pinned revisions asked for.
    CHECK(w.store.object_count() == objects);
    CHECK(w.store.last_seq() == seq);
    for (const auto &[package, revision] : provider.asked) {
      CHECK(((package == p1 && revision == pin_item(p1, pkg1, 0).v.revision) ||
             (package == p2 && revision == pin_item(p2, pkg2, 0).v.revision)));
    }
  }
}
