// testing::fabric_host and testing::link_rig (WO-097). See fabric_host.hpp.

#include "fabric_host.hpp"

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
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/time.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::testing {

namespace fab = sl::fabric;

namespace {

fab::field_desc make_field(std::uint32_t no, std::string_view name, std::string_view type,
                           fab::presence p = fab::presence::required) {
  fab::field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = fab::type_id_of(type);
  f.presence = p;
  return f;
}

fab::type_desc make_type(std::string_view name, fab::type_kind kind) {
  fab::type_desc t;
  t.name = std::string(name);
  t.id = fab::type_id_of(name);
  t.kind = kind;
  t.domain = std::string(name.substr(0, name.rfind('.')));
  return t;
}

fab::type_desc make_record(std::string_view name, bool root) {
  fab::type_desc t = make_type(name, fab::type_kind::struct_);
  t.record_class = fab::id_class::entity;
  t.state_class = fab::state_class::document;
  t.document_root = root;
  return t;
}

result<void> add_types(fab::schema_registry &reg) {
  fab::type_desc members = make_type(host_member_list_type, fab::type_kind::oset);
  members.element = fab::type_id_of("ref");

  fab::type_desc level = make_record(host_level_type, true);
  fab::field_desc owned = make_field(2, "members", host_member_list_type);
  owned.ref = fab::ref_spec{
      .strength = fab::ref_strength::owned,
      .targets = {fab::type_id_of(host_body_type), fab::type_id_of(host_marker_type)}};
  level.fields = {make_field(1, "name", "string"), owned};

  fab::type_desc body = make_record(host_body_type, false);
  fab::field_desc heat = make_field(4, "heat", "i32", fab::presence::optional);
  heat.default_value = fab::value::integer(0);
  body.fields = {make_field(1, "mass", "f32"), make_field(2, "count", "u32"),
                 make_field(3, "alive", "bool"), heat,
                 make_field(5, "label", "string", fab::presence::optional)};

  fab::type_desc marker = make_record(host_marker_type, false);
  marker.fields = {make_field(1, "level", "u32")};

  fab::type_desc crate = make_record(host_crate_type, true);
  crate.fields = {make_field(1, "weight", "f32")};

  for (fab::type_desc t : {members, level, body, marker, crate}) {
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

// The fixed order key of member `i` of a level (valid: two bytes, the last non-zero).
fab::order_key member_key(std::uint32_t i) {
  return {static_cast<std::uint8_t>(1U + (i / 255U)), static_cast<std::uint8_t>(1U + (i % 255U))};
}

// The element id of member `id` in its level's member list: an element-class id derived from the
// record id's bytes (unique per record, the same for create and retire).
fab::id_bytes element_of(const fab::record_id &id) {
  const auto &raw = id.bytes();
  const std::string key(raw.begin(), raw.end());
  return fab::derive(fab::id_class::element, fab::well_known(fab::id_class::namespace_, 1), key)
      .bytes();
}

fab::tx_origin editor_origin() {
  fab::tx_origin o;
  o.actor = fabric_host::editor_actor();
  o.subsystem = "content";
  return o;
}

std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

result<fab::commit_receipt> commit_as(fab::workspace &ws, const fab::tx_origin &origin,
                                      const std::vector<fab::patch_op> &ops) {
  fab::tx_options o;
  o.origin = origin;
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

// One seeded member: create it and insert it into its level.
struct seed_member {
  fab::record_id id;
  fab::type_id type;
  fab::value body;
  std::uint32_t level = 0;
  std::uint32_t slot = 0;
};

} // namespace

fab::record_id fabric_host::level_id(std::uint32_t i) {
  return named(std::format("bnd/level/{}", i));
}
fab::record_id fabric_host::body_id(std::uint32_t i) {
  return named(std::format("bnd/body/{}", i));
}
fab::record_id fabric_host::marker_id(std::uint32_t i) {
  return named(std::format("bnd/marker/{}", i));
}
fab::record_id fabric_host::crate_id(std::uint32_t i) {
  return named(std::format("bnd/crate/{}", i));
}
fab::actor_id fabric_host::runtime_actor() noexcept { return actor_named("bnd/runtime"); }
fab::actor_id fabric_host::editor_actor() noexcept { return actor_named("bnd/editor"); }

bnd_body fabric_host::seed_body(std::uint32_t index) noexcept {
  return bnd_body{.mass = 1.0F + static_cast<float>(index),
                  .count = index * 3U,
                  .alive = index % 2U == 0U,
                  .heat = static_cast<std::int32_t>(index % 7U) - 3,
                  .spare = 0};
}

result<fab::value> fabric_host::body_value(const bnd_body &v) {
  auto mass = fab::value::f32(v.mass);
  if (!mass) {
    return std::unexpected(mass.error());
  }
  return fab::value::struct_({fab::field{.no = 1, .v = *mass},
                              fab::field{.no = 2, .v = fab::value::uint(v.count)},
                              fab::field{.no = 3, .v = fab::value::boolean(v.alive)},
                              fab::field{.no = 4, .v = fab::value::integer(v.heat)}});
}

fab::patch_op fabric_host::set_body_field(std::uint32_t index, std::uint32_t no, fab::value v) {
  return fab::op_set(body_id(index), {fab::path_segment::field(no)}, std::move(v));
}

result<std::vector<fab::patch_op>>
fabric_host::create_body_ops(std::uint32_t level, std::uint32_t index, const bnd_body &v) {
  auto body = body_value(v);
  if (!body) {
    return std::unexpected(body.error());
  }
  const fab::record_id id = body_id(index);
  return std::vector<fab::patch_op>{
      fab::op_create(id, fab::type_id_of(host_body_type), std::move(*body)),
      fab::op_insert(level_id(level), {fab::path_segment::field(2)}, element_of(id),
                     member_key(index), fab::value::id(id))};
}

std::vector<fab::patch_op> fabric_host::retire_body_ops(std::uint32_t level, std::uint32_t index) {
  const fab::record_id id = body_id(index);
  return {fab::op_remove(level_id(level), {fab::path_segment::field(2)}, element_of(id)),
          fab::op_delete(id)};
}

result<void> fabric_host::seed(fab::workspace &ws, const host_spec &spec) {
  if (spec.levels == 0 && (spec.bodies != 0 || spec.markers != 0)) {
    return make_error(errc::invalid_argument, "fabric_host: members need at least one level");
  }
  std::vector<fab::patch_op> roots;
  for (std::uint32_t d = 0; d < spec.levels; ++d) {
    auto name = fab::value::string(std::format("level {}", d));
    auto list = fab::value::oset({});
    if (!name || !list) {
      return make_error(errc::internal, "fabric_host: a level value");
    }
    auto level =
        fab::value::struct_({fab::field{.no = 1, .v = *name}, fab::field{.no = 2, .v = *list}});
    if (!level) {
      return std::unexpected(level.error());
    }
    roots.push_back(fab::op_create(level_id(d), fab::type_id_of(host_level_type), *level));
  }
  for (std::uint32_t c = 0; c < spec.crates; ++c) {
    auto weight = fab::value::f32(2.0F + static_cast<float>(c));
    if (!weight) {
      return std::unexpected(weight.error());
    }
    auto crate = fab::value::struct_({fab::field{.no = 1, .v = *weight}});
    if (!crate) {
      return std::unexpected(crate.error());
    }
    roots.push_back(fab::op_create(crate_id(c), fab::type_id_of(host_crate_type), *crate));
  }
  if (!roots.empty()) {
    if (auto ok = commit_as(ws, editor_origin(), roots); !ok) {
      return std::unexpected(ok.error());
    }
  }
  std::vector<seed_member> members;
  for (std::uint32_t i = 0; i < spec.bodies; ++i) {
    auto body = body_value(seed_body(i));
    if (!body) {
      return std::unexpected(body.error());
    }
    members.push_back(seed_member{.id = body_id(i),
                                  .type = fab::type_id_of(host_body_type),
                                  .body = std::move(*body),
                                  .level = i % spec.levels,
                                  .slot = i});
  }
  for (std::uint32_t i = 0; i < spec.markers; ++i) {
    auto marker = fab::value::struct_({fab::field{.no = 1, .v = fab::value::uint(i)}});
    if (!marker) {
      return std::unexpected(marker.error());
    }
    members.push_back(seed_member{.id = marker_id(i),
                                  .type = fab::type_id_of(host_marker_type),
                                  .body = std::move(*marker),
                                  .level = i % spec.levels,
                                  .slot = spec.bodies + i});
  }
  // The seed decides the commit order and the batching, never the state.
  std::uint64_t r = mix(spec.seed);
  for (std::size_t i = members.size(); i > 1; --i) {
    r = mix(r);
    std::swap(members[i - 1], members[r % i]);
  }
  std::size_t next = 0;
  while (next < members.size()) {
    r = mix(r);
    const std::size_t batch = 1 + (r % 4);
    std::vector<fab::patch_op> ops;
    for (std::size_t k = 0; k < batch && next < members.size(); ++k, ++next) {
      const seed_member &m = members[next];
      ops.push_back(fab::op_create(m.id, m.type, m.body));
      ops.push_back(fab::op_insert(level_id(m.level), {fab::path_segment::field(2)},
                                   element_of(m.id), member_key(m.slot), fab::value::id(m.id)));
    }
    if (auto ok = commit_as(ws, editor_origin(), ops); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return {};
}

result<fab::document_projection> fabric_host::head_projection(const fab::workspace &ws,
                                                              const host_spec &spec) {
  auto head = ws.manager().head();
  if (!head) {
    return std::unexpected(head.error());
  }
  return fab::document_projection::make(*head, ws.schemas(), spec.domains);
}

result<std::unique_ptr<fabric_host>> fabric_host::make(const host_spec &spec) {
  fab::workspace_options o;
  if (auto ok = add_types(o.schemas); !ok) {
    return std::unexpected(ok.error());
  }
  for (std::string_view domain : {"content.scene", "content.prop"}) {
    if (auto ok = o.domains.claim(fab::domain_spec{.name = std::string(domain),
                                                   .owner = fab::subsystem::content,
                                                   .state_classes = {fab::state_class::document},
                                                   .types = {}});
        !ok) {
      return std::unexpected(ok.error());
    }
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
  o.ids = std::make_shared<fab::deterministic_source>(spec.seed, "bnd-host");
  auto ws = fab::workspace::create_memory(std::move(o));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  if (auto ok = seed(**ws, spec); !ok) {
    return std::unexpected(ok.error());
  }
  auto head = head_projection(**ws, spec);
  if (!head) {
    return std::unexpected(head.error());
  }
  return std::make_unique<fabric_host>(passkey{}, std::move(*ws), spec, std::move(*head));
}

fabric_host::fabric_host(passkey /*key*/, std::unique_ptr<fab::workspace> ws, host_spec spec,
                         fab::document_projection head) noexcept
    : ws_(std::move(ws)), spec_(std::move(spec)), projection_(std::move(head)) {}

result<void> fabric_host::refresh(bool post) {
  auto head = head_projection(*ws_, spec_);
  if (!head) {
    return std::unexpected(head.error());
  }
  projection_ = std::move(*head);
  if (post && link_ != nullptr) {
    return link_->post_projection(projection_);
  }
  return {};
}

result<void> fabric_host::submit(const fab::patch &p, const fab::tx_origin &origin) noexcept {
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
  last_receipt_ = std::move(*receipt);
  return refresh(true);
}

result<fab::commit_receipt> fabric_host::edit(const std::vector<fab::patch_op> &ops, bool post) {
  auto receipt = commit_as(*ws_, editor_origin(), ops);
  if (!receipt) {
    return std::unexpected(receipt.error());
  }
  if (auto ok = refresh(post); !ok) {
    return std::unexpected(ok.error());
  }
  return receipt;
}

// --- link_rig ---

namespace {

type_info body_info() {
  type_builder<bnd_body> b(host_body_type);
  SL_RT_FIELD_BOUND(b, bnd_body, mass, field_flags::snapshot, 1);
  SL_RT_FIELD_BOUND(b, bnd_body, count, field_flags::snapshot, 2);
  SL_RT_FIELD_BOUND(b, bnd_body, alive, field_flags::snapshot, 3);
  SL_RT_FIELD_BOUND(b, bnd_body, heat, field_flags::snapshot, 4);
  SL_RT_FIELD(b, bnd_body, spare, field_flags::transient);
  b.schema(host_body_type);
  return b.build();
}

type_info marker_info() {
  type_builder<bnd_marker> b(host_marker_type);
  SL_RT_FIELD_BOUND(b, bnd_marker, level, field_flags::snapshot, 1);
  b.schema(host_marker_type);
  return b.build();
}

type_info crate_info() {
  type_builder<bnd_crate> b(host_crate_type);
  SL_RT_FIELD_BOUND(b, bnd_crate, weight, field_flags::snapshot, 1);
  b.schema(host_crate_type);
  return b.build();
}

type_info spin_info() {
  type_builder<bnd_spin> b("bnd.Spin");
  SL_RT_FIELD(b, bnd_spin, rate, field_flags::transient);
  return b.build();
}

result<void> bind(link_rig &r, ecs::component_id component, std::string_view name) {
  auto t = r.types.find(name);
  if (!t) {
    return std::unexpected(t.error());
  }
  auto b = fabric_binding::bind(**t, r.host->schemas(), name);
  if (!b) {
    return std::unexpected(b.error());
  }
  return r.bindings.add(component, std::move(*b));
}

result<void> finish_stack(link_rig &r, std::uint32_t workers) {
  auto stack = std::make_unique<sched_rig>();
  auto started = sched_rig::start_jobs(stack->plat.get(), workers);
  if (!started) {
    return std::unexpected(started.error());
  }
  stack->own_jobs = std::move(*started);
  stack->js = stack->own_jobs.get();
  stack->arena = std::make_unique<mem::frame_arena>(stack->upstream, workers, 64U << 10U);
  frame_config fc;
  fc.rate = time::tick_rate{.num = 50, .den = 1};
  auto frames = frame_scheduler::make(*stack->js, stack->plat.get(), *stack->arena, fc);
  if (!frames) {
    return std::unexpected(frames.error());
  }
  stack->frames = std::move(*frames);
  auto body = stack->registry.add<bnd_body>("bnd.body");
  auto marker = stack->registry.add<bnd_marker>("bnd.marker");
  auto crate = stack->registry.add<bnd_crate>("bnd.crate");
  auto spin = stack->registry.add<bnd_spin>("bnd.spin");
  if (!body || !marker || !crate || !spin) {
    return make_error(errc::internal, "link_rig: component registration");
  }
  r.body_component = *body;
  r.marker_component = *marker;
  r.crate_component = *crate;
  r.spin_component = *spin;
  if (auto frozen = stack->registry.freeze(); !frozen) {
    return frozen;
  }
  auto w = ecs::world::make(stack->registry, stack->world_memory);
  if (!w) {
    return std::unexpected(w.error());
  }
  stack->world = std::move(*w);
  stack->queue = std::make_unique<ecs::command_queue>(stack->registry, *stack->arena);
  stack->systems = std::make_unique<ecs::system_scheduler>(*stack->js);
  r.stack = std::move(stack);
  return {};
}

} // namespace

result<std::unique_ptr<link_rig>> link_rig::make(const rig_options &options) {
  auto r = std::make_unique<link_rig>();
  auto host = fabric_host::make(options.host);
  if (!host) {
    return std::unexpected(host.error());
  }
  r->host = std::move(*host);
  if (auto ok = finish_stack(*r, options.workers); !ok) {
    return std::unexpected(ok.error());
  }
  auto tag = r->tags.reserve_external(link::projection_tag_name);
  if (!tag) {
    return std::unexpected(tag.error());
  }
  r->projection_tag = *tag;
  r->diagnostics = std::make_unique<mem::diagnostics>(r->tags, 1);
  for (const type_info &t : {body_info(), marker_info(), crate_info(), spin_info()}) {
    if (auto added = r->types.add(t); !added) {
      return std::unexpected(added.error());
    }
  }
  if (auto frozen = r->types.freeze(); !frozen) {
    return std::unexpected(frozen.error());
  }
  if (options.bind_body) {
    if (auto ok = bind(*r, r->body_component, host_body_type); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (options.bind_marker) {
    if (auto ok = bind(*r, r->marker_component, host_marker_type); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (options.bind_crate) {
    if (auto ok = bind(*r, r->crate_component, host_crate_type); !ok) {
      return std::unexpected(ok.error());
    }
  }
  link::link_config config;
  config.actor = fabric_host::runtime_actor();
  config.domains = options.host.domains;
  config.workspace_alive = r->host->alive_token();
  config.projection_tag = r->projection_tag;
  link::proposal_sink *sink = r->host.get();
  if (options.preview_sink) {
    sink = &r->preview;
  }
  auto attached = link::world_link::attach(*r->stack->world, *r->stack->frames, *r->stack->systems,
                                           *r->stack->queue, *r->diagnostics, r->bindings, *sink,
                                           std::move(config));
  if (!attached) {
    return std::unexpected(attached.error());
  }
  r->link = std::move(*attached);
  r->host->attach(*r->link);
  return r;
}

result<link::link_stats> link_rig::ingest() const {
  auto source = link::document_source::make(host->projection());
  if (!source) {
    return std::unexpected(source.error());
  }
  return link->ingest(*source);
}

result<void> link_rig::build() const {
  if (auto built = stack->build(); !built) {
    return built;
  }
  auto primed = stack->step();
  if (!primed) {
    return std::unexpected(primed.error());
  }
  return {};
}

result<frame_stats> link_rig::step() const { return stack->step(); }

result<ecs::entity> link_rig::entity_of(const fab::record_id &record) const {
  return link->entities().find(record);
}

std::uint64_t link_rig::external_projection_bytes() const {
  const mem::memory_report report = diagnostics->make_report(0);
  for (const mem::external_stats &e : report.external) {
    // cppcheck-suppress useStlAlgorithm ; a plain loop over a short list reads plainer
    if (e.tag == link::projection_tag_name) {
      return e.bytes;
    }
  }
  return 0;
}

} // namespace sl::runtime::testing
