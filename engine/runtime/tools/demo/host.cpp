// The lantern-field host (WO-099). See host.hpp.

#include "host.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"
#include "sl/runtime/bridge.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::demo {

namespace fab = sl::fabric;

namespace {

fab::field_desc make_field(std::uint32_t no, std::string_view name, std::string_view type) {
  fab::field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = fab::type_id_of(type);
  f.presence = fab::presence::required;
  return f;
}

fab::type_desc make_record(std::string_view name) {
  fab::type_desc t;
  t.name = std::string(name);
  t.id = fab::type_id_of(name);
  t.kind = fab::type_kind::struct_;
  t.domain = std::string(scene_domain);
  t.record_class = fab::id_class::entity;
  t.state_class = fab::state_class::document;
  t.document_root = true;
  return t;
}

result<void> add_types(fab::schema_registry &reg) {
  fab::type_desc beacon = make_record(beacon_type);
  beacon.fields = {make_field(1, "glow", "f32"), make_field(2, "lit", "u32")};
  fab::type_desc ward = make_record(ward_type);
  ward.fields = {make_field(1, "radius", "f32"), make_field(2, "strength", "f32")};
  for (fab::type_desc t : {beacon, ward}) {
    if (auto ok = reg.add(std::move(t)); !ok) {
      return ok;
    }
  }
  return reg.freeze();
}

fab::record_id named(std::string_view key) noexcept {
  return fab::derive(fab::id_class::entity, fab::well_known(fab::id_class::namespace_, 1), key);
}

fab::actor_id actor_named(std::string_view key) noexcept {
  return fab::actor_id::from(
             fab::derive(fab::id_class::actor, fab::well_known(fab::id_class::namespace_, 1), key))
      .value_or(fab::actor_id{});
}

result<fab::commit_receipt> commit_as(fab::workspace &ws, const std::vector<fab::patch_op> &ops) {
  fab::tx_options o;
  o.origin.actor = lantern_host::editor_actor();
  o.origin.subsystem = "content";
  o.sync = fab::durability::none;
  auto t = ws.manager().begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const fab::patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return t->commit();
}

result<fab::value> pair_value(float first, fab::value second) {
  auto a = fab::value::f32(first);
  if (!a) {
    return std::unexpected(a.error());
  }
  return fab::value::struct_(
      {fab::field{.no = 1, .v = *a}, fab::field{.no = 2, .v = std::move(second)}});
}

// The seeded records: wards, then beacons, in commits of 64 records (4 commits).
result<std::uint64_t> seed(fab::workspace &ws) {
  std::vector<fab::patch_op> ops;
  std::uint64_t commits = 0;
  const auto flush = [&] -> result<void> {
    if (ops.empty()) {
      return {};
    }
    auto ok = commit_as(ws, ops);
    if (!ok) {
      return std::unexpected(ok.error());
    }
    ops.clear();
    ++commits;
    return {};
  };
  for (std::uint32_t i = 0; i < ward_records; ++i) {
    auto strength = fab::value::f32(0.5F + (static_cast<float>(i % 5U) * 0.25F));
    if (!strength) {
      return std::unexpected(strength.error());
    }
    auto v = pair_value(4.0F + static_cast<float>(i % 8U), std::move(*strength));
    if (!v) {
      return std::unexpected(v.error());
    }
    ops.push_back(fab::op_create(lantern_host::ward_id(i), fab::type_id_of(ward_type), *v));
    if (ops.size() == 64) {
      if (auto ok = flush(); !ok) {
        return std::unexpected(ok.error());
      }
    }
  }
  for (std::uint32_t i = 0; i < beacon_records; ++i) {
    auto v = pair_value(1.0F + (static_cast<float>(i) * 0.5F), fab::value::uint(0));
    if (!v) {
      return std::unexpected(v.error());
    }
    ops.push_back(fab::op_create(lantern_host::beacon_id(i), fab::type_id_of(beacon_type), *v));
    if (ops.size() == 64) {
      if (auto ok = flush(); !ok) {
        return std::unexpected(ok.error());
      }
    }
  }
  if (auto ok = flush(); !ok) {
    return std::unexpected(ok.error());
  }
  return commits;
}

result<fab::document_projection> head_projection(const fab::workspace &ws) {
  auto head = ws.manager().head();
  if (!head) {
    return std::unexpected(head.error());
  }
  const std::vector<std::string> domains{std::string(scene_domain)};
  return fab::document_projection::make(*head, ws.schemas(), domains);
}

} // namespace

fab::actor_id lantern_host::runtime_actor() noexcept { return actor_named("gate/runtime"); }
fab::actor_id lantern_host::editor_actor() noexcept { return actor_named("gate/editor"); }
fab::record_id lantern_host::beacon_id(std::uint32_t i) {
  return named(std::format("gate/beacon/{}", i));
}
fab::record_id lantern_host::ward_id(std::uint32_t i) {
  return named(std::format("gate/ward/{}", i));
}

result<std::unique_ptr<lantern_host>> lantern_host::make(std::uint64_t seed_value) {
  fab::workspace_options o;
  if (auto ok = add_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = o.domains.claim(fab::domain_spec{.name = std::string(scene_domain),
                                                 .owner = fab::subsystem::content,
                                                 .state_classes = {fab::state_class::document},
                                                 .types = {}});
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = o.domains.bind_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  for (const fab::actor &a :
       {fab::actor{.id = editor_actor(), .kind = fab::actor_kind::human, .grants = {}},
        fab::actor{.id = runtime_actor(),
                   .kind = fab::actor_kind::service,
                   .grants = {"write:content.scene"}}}) {
    if (auto ok = o.actors.add(a); !ok) {
      return std::unexpected(ok.error());
    }
  }
  o.clock = std::make_shared<fab::manual_clock>(1000);
  o.ids = std::make_shared<fab::deterministic_source>(seed_value, "gate-host");
  auto ws = fab::workspace::create_memory(std::move(o));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  auto commits = seed(**ws);
  if (!commits) {
    return std::unexpected(commits.error());
  }
  auto head = head_projection(**ws);
  if (!head) {
    return std::unexpected(head.error());
  }
  return std::make_unique<lantern_host>(passkey{}, std::move(*ws), std::move(*head), *commits);
}

lantern_host::lantern_host(passkey /*key*/, std::unique_ptr<fab::workspace> ws,
                           fab::document_projection head, std::uint64_t seed_commits) noexcept
    : ws_(std::move(ws)), projection_(std::move(head)), seed_commits_(seed_commits) {}

result<void> lantern_host::submit(const fab::patch &p, const fab::tx_origin &origin) noexcept {
  fab::tx_options o;
  o.origin = origin;
  o.base = p.base;
  o.policy = fab::commit_policy::rebase_if_commutes;
  o.sync = fab::durability::none; // the memory backend's durability
  auto t = ws_->manager().begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  if (auto ok = t->stage(p); !ok) {
    return std::unexpected(ok.error());
  }
  auto receipt = t->commit();
  if (!receipt) {
    return std::unexpected(receipt.error());
  }
  ++commits_;
  auto head = head_projection(*ws_);
  if (!head) {
    return std::unexpected(head.error());
  }
  projection_ = std::move(*head);
  if (link_ != nullptr) {
    return link_->post_projection(projection_);
  }
  return {};
}

} // namespace sl::runtime::demo
