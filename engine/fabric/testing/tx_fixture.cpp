// Helpers for the transaction-manager tests (WO-050).

#include "tx_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_schemas.hpp"
#include "tree_fixture.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

namespace {

actor_id named_actor(std::string_view name) noexcept {
  return actor_id::from(test_id(id_class::actor, name)).value_or(actor_id{});
}

digest digest_of(std::string_view text) noexcept {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

result<actor_directory> test_actors() {
  actor_directory dir;
  for (auto [id, kind] :
       {std::pair{tx_editor(), actor_kind::human}, std::pair{tx_agent(), actor_kind::agent},
        std::pair{tx_other(), actor_kind::human}}) {
    if (auto ok = dir.add(actor{.id = id, .kind = kind, .grants = {}}); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return dir;
}

} // namespace

actor_id tx_editor() noexcept { return named_actor("tx/editor"); }
actor_id tx_agent() noexcept { return named_actor("tx/agent"); }
actor_id tx_other() noexcept { return named_actor("tx/other"); }

tx_origin tx_origin_of(const actor_id &actor, std::string_view subsystem) {
  tx_origin o;
  o.actor = actor;
  o.subsystem = std::string(subsystem);
  return o;
}

result<workspace_options> scene_options(std::shared_ptr<const clock> clock, std::uint64_t seed) {
  workspace_options o;
  if (auto ok = add_test_schemas(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = o.schemas.freeze(); !ok) {
    return std::unexpected(ok.error());
  }
  const std::vector<domain_spec> claims = {
      {.name = "content.scene",
       .owner = subsystem::content,
       .state_classes = {state_class::document},
       .types = {}},
      {.name = "content.mesh",
       .owner = subsystem::content,
       .state_classes = {state_class::document},
       .types = {}},
      {.name = "asset.cooked",
       .owner = subsystem::asset,
       .state_classes = {state_class::derived},
       .types = {}},
  };
  for (const domain_spec &c : claims) {
    if (auto ok = o.domains.claim(c); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (auto ok = o.domains.bind_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  auto actors = test_actors();
  if (!actors) {
    return std::unexpected(actors.error());
  }
  o.actors = std::move(*actors);
  o.clock = std::move(clock);
  o.ids = std::make_shared<deterministic_source>(seed, "tx-test");
  return o;
}

result<workspace_options> gen_options(std::shared_ptr<const clock> clock, std::uint64_t seed) {
  workspace_options o;
  if (auto ok = add_tree_record_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  auto actors = test_actors();
  if (!actors) {
    return std::unexpected(actors.error());
  }
  o.actors = std::move(*actors);
  o.clock = std::move(clock);
  o.ids = std::make_shared<deterministic_source>(seed, "tx-gen");
  o.omit_builtin = {"fabric.schema", "fabric.authority", "fabric.references", "fabric.invariants"};
  return o;
}

result<commit_receipt> commit_ops(transaction_manager &m, const tx_origin &origin,
                                  const std::vector<patch_op> &ops, patch_meta meta,
                                  std::string_view ref) {
  tx_options o;
  o.origin = origin;
  o.ref = std::string(ref);
  o.meta = std::move(meta);
  auto t = m.begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return t->commit();
}

record_id entity_named(std::string_view name) noexcept { return test_id(id_class::entity, name); }
record_id asset_named(std::string_view name) noexcept { return test_id(id_class::asset, name); }
record_id derived_named(std::string_view name) noexcept { return test_id(id_class::derived, name); }

type_id entity_type() noexcept { return type_id_of(test_types::entity); }
type_id scene_type() noexcept { return type_id_of(test_types::scene); }
type_id mesh_type() noexcept { return type_id_of(test_types::mesh); }
type_id cooked_type() noexcept { return type_id_of(test_types::cooked_mesh); }

result<patch_op> create_entity(std::string_view name) {
  auto body = sample_entity(name);
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_create(entity_named(name), entity_type(), std::move(*body));
}

result<patch_op> create_scene(const record_id &id, std::string_view name,
                              const std::vector<record_id> &members) {
  std::vector<oset_entry> entries;
  std::vector<std::uint8_t> key;
  for (const record_id &m : members) {
    auto next = order_key_between(key, {});
    if (!next) {
      return std::unexpected(next.error());
    }
    key = *next;
    entries.push_back(oset_entry{
        .element = derive(id_class::element, id, m.to_string()).bytes(),
        .order_key = key,
        .v = value::id(m),
    });
  }
  auto label = value::string(name);
  if (!label) {
    return std::unexpected(label.error());
  }
  auto list = value::oset(std::move(entries));
  if (!list) {
    return std::unexpected(list.error());
  }
  auto body = value::struct_({field{.no = 1, .v = *label}, field{.no = 2, .v = *list}});
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_create(id, scene_type(), std::move(*body));
}

result<patch_op> create_mesh(std::string_view name, std::uint32_t vertices) {
  auto system = value::string("file");
  auto locator = value::string(std::string("meshes/") + std::string(name) + ".glb");
  if (!system || !locator) {
    return make_error(errc::invalid_argument, "bad mesh name");
  }
  auto source = value::struct_({field{.no = 1, .v = *system}, field{.no = 2, .v = *locator}});
  if (!source) {
    return std::unexpected(source.error());
  }
  auto body =
      value::struct_({field{.no = 1, .v = *source}, field{.no = 2, .v = value::uint(vertices)}});
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_create(asset_named(name), mesh_type(), std::move(*body));
}

result<patch_op> create_cooked(std::string_view name, const record_id &source) {
  auto body = value::struct_(
      {field{.no = 1, .v = value::id(source)},
       field{.no = 2, .v = value::digest(digest_of(std::string("payload/") + std::string(name)))}});
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_create(derived_named(name), cooked_type(), std::move(*body));
}

result<patch_op> set_name(const record_id &id, std::string_view name) {
  auto v = value::string(name);
  if (!v) {
    return std::unexpected(v.error());
  }
  return op_set(id, {path_segment::field(1)}, std::move(*v));
}

provenance complete_provenance(const record_id &input) {
  provenance p;
  p.input_ids = std::vector<record_id>{input};
  p.input_revisions = std::vector<digest>{digest_of("input/" + input.to_string())};
  p.toolchain_id = "sl.cook/1";
  p.parameters_digest = digest_of("parameters");
  p.environment_hash = digest_of("environment");
  return p;
}

result<oracle_store> oracle_of(const snapshot &s) {
  oracle_store out;
  auto walked = s.for_each_entry([&out](const snapshot_entry &e) -> result<void> {
    out.put(e.id, record_state{.type = e.type, .body = e.body});
    return {};
  });
  if (!walked) {
    return std::unexpected(walked.error());
  }
  return out;
}

patch load_patch(const oracle_store &state) {
  patch p;
  for (const auto &[id, r] : state.records()) {
    p.ops.push_back(op_create(id, r.type, r.body));
  }
  return p;
}

result<std::map<std::string, std::vector<std::uint8_t>>>
file_image(vfs_memory &fs, const std::filesystem::path &dir) {
  std::map<std::string, std::vector<std::uint8_t>> out;
  std::vector<std::filesystem::path> todo{dir};
  while (!todo.empty()) {
    const std::filesystem::path cur = todo.back();
    todo.pop_back();
    auto names = fs.list(cur);
    if (!names) {
      return std::unexpected(names.error());
    }
    for (const std::string &n : *names) {
      const std::filesystem::path child = cur / n;
      auto bytes = fs.contents(child);
      if (bytes) {
        out.insert_or_assign(child.generic_string(), std::move(*bytes));
      } else {
        todo.push_back(child); // a directory
      }
    }
  }
  return out;
}

tx_log_store::tx_log_store() : fs_(std::make_unique<faulty_vfs>()), dir_("/ws/.slfabric") {}

result<std::shared_ptr<store_backend>> tx_log_store::open() {
  if (!made_) {
    // The workspace directory exists durably before the store is created inside it.
    if (auto ok = fs_->make_dir(dir_.parent_path()); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = fs_->sync_dir("/"); !ok) {
      return std::unexpected(ok.error());
    }
    made_ = true;
  }
  log_options options;
  options.sync = durability::fsync;
  auto s = log_backend::open(*fs_, dir_, options);
  if (!s) {
    return std::unexpected(s.error());
  }
  return std::shared_ptr<store_backend>(std::move(*s));
}

} // namespace sl::fabric::testing
