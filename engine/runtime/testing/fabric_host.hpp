#pragma once

// testing::fabric_host (WO-097, SL-ARCH-003 ARCH3-R149, R156, R159, R161; ADR-0027): the host side
// of the Fabric link for tests and the gate. It owns a memory workspace whose content types are
// registered in code through the public schema.hpp:
//   content.scene.Level       record (entity, document, document root) {1 name string,
//                             2 members MemberList owned -> Body | Marker}
//   content.scene.MemberList  oset of ref
//   content.scene.Body        record (entity, document) {1 mass f32, 2 count u32, 3 alive bool,
//                             4 heat i32 optional default 0, 5 label string optional}
//   content.scene.Marker      record (entity, document) {1 level u32}
//   content.prop.Crate        record (entity, document, document root) {1 weight f32}
// The editor (a human actor, subsystem content) seeds `levels` documents, `bodies` and `markers`
// round-robin over them and `crates` (each its own document in content.prop), committed in a
// seeded order and batching (the state, ids and order keys do not depend on the seed). The runtime
// actor is a service actor holding write:content.scene only. submit commits a proposal with the
// given origin over the patch base under commit_policy::rebase_if_commutes and the memory
// backend's durability (none), then posts the new head projection to the attached link. The F5
// ledger is read by the tests themselves (f5.hpp never appears under engine/runtime).
// testing::link_rig is the whole stack under a link: the host, a sched_rig with the bnd_*
// components, tags with fabric.projection reserved, diagnostics, bindings and the world_link.
// Owner-thread (`thread_safety`, testing::*). Include it by path:
// "../../../engine/runtime/testing/fabric_host.hpp".

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"

#include "sim_fixture.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sl::runtime::testing {

inline constexpr std::string_view host_level_type = "content.scene.Level";
inline constexpr std::string_view host_member_list_type = "content.scene.MemberList";
inline constexpr std::string_view host_body_type = "content.scene.Body";
inline constexpr std::string_view host_marker_type = "content.scene.Marker";
inline constexpr std::string_view host_crate_type = "content.prop.Crate";

/// The runtime side of content.scene.Body: fields 1 to 4 bound, `spare` transient.
struct bnd_body {
  float mass = 0.0F;
  std::uint32_t count = 0;
  bool alive = false;
  std::int32_t heat = 0;
  std::uint32_t spare = 0;
};
/// content.scene.Marker {1 level}.
struct bnd_marker {
  std::uint32_t level = 0;
};
/// content.prop.Crate {1 weight}.
struct bnd_crate {
  float weight = 0.0F;
};
/// A transient component: no binding, never observed.
struct bnd_spin {
  float rate = 0.0F;
};

/// What fabric_host seeds.
struct host_spec {
  std::uint32_t levels = 2;
  std::uint32_t bodies = 8;
  std::uint32_t markers = 0;
  std::uint32_t crates = 0;
  std::vector<std::string> domains{"content.scene"};
  /// The commit order and batching of the seed (not the state).
  std::uint64_t seed = 1;
};

class fabric_host final : public link::proposal_sink {
  struct passkey {};

public:
  /// A memory workspace with the types above, seeded per `spec`; the workspace's errors.
  [[nodiscard]] static result<std::unique_ptr<fabric_host>> make(const host_spec &spec);

  fabric_host(passkey /*key*/, std::unique_ptr<sl::fabric::workspace> ws, host_spec spec,
              sl::fabric::document_projection head) noexcept;
  fabric_host(const fabric_host &) = delete;
  fabric_host(fabric_host &&) = delete;
  fabric_host &operator=(const fabric_host &) = delete;
  fabric_host &operator=(fabric_host &&) = delete;
  ~fabric_host() override = default;

  /// The head of main over spec.domains.
  [[nodiscard]] sl::fabric::document_projection projection() const { return projection_; }
  /// Commits `p` by `origin` on p.base under rebase_if_commutes, then posts the head projection to
  /// the attached link. The manager's errors (authority_denied for a domain the actor may not
  /// write).
  [[nodiscard]] result<void> submit(const sl::fabric::patch &p,
                                    const sl::fabric::tx_origin &origin) noexcept override;
  /// Successful submits.
  [[nodiscard]] std::uint64_t commits() const noexcept { return commits_; }
  /// The link the head projection is posted to after each commit.
  void attach(link::world_link &l) noexcept { link_ = &l; }

  // --- for tests ---

  /// Commits `ops` as the editor; posts the head projection to the attached link when `post`.
  [[nodiscard]] result<sl::fabric::commit_receipt>
  edit(const std::vector<sl::fabric::patch_op> &ops, bool post = true);
  /// The ops creating Body `index` with `v` in Level `level`, and retiring it.
  [[nodiscard]] static result<std::vector<sl::fabric::patch_op>>
  create_body_ops(std::uint32_t level, std::uint32_t index, const bnd_body &v);
  [[nodiscard]] static std::vector<sl::fabric::patch_op> retire_body_ops(std::uint32_t level,
                                                                         std::uint32_t index);
  /// set Body `index` field `no` to `v`.
  [[nodiscard]] static sl::fabric::patch_op set_body_field(std::uint32_t index, std::uint32_t no,
                                                           sl::fabric::value v);
  /// A Body value with fields 1 to 4.
  [[nodiscard]] static result<sl::fabric::value> body_value(const bnd_body &v);
  /// The seeded value of Body `index`.
  [[nodiscard]] static bnd_body seed_body(std::uint32_t index) noexcept;

  [[nodiscard]] static sl::fabric::record_id level_id(std::uint32_t i);
  [[nodiscard]] static sl::fabric::record_id body_id(std::uint32_t i);
  [[nodiscard]] static sl::fabric::record_id marker_id(std::uint32_t i);
  [[nodiscard]] static sl::fabric::record_id crate_id(std::uint32_t i);
  [[nodiscard]] static sl::fabric::actor_id runtime_actor() noexcept;
  [[nodiscard]] static sl::fabric::actor_id editor_actor() noexcept;

  [[nodiscard]] const sl::fabric::workspace &workspace() const noexcept { return *ws_; }
  [[nodiscard]] const sl::fabric::schema_registry &schemas() const noexcept {
    return ws_->schemas();
  }
  [[nodiscard]] const host_spec &spec() const noexcept { return spec_; }
  [[nodiscard]] const std::optional<sl::fabric::commit_receipt> &last_receipt() const noexcept {
    return last_receipt_;
  }
  /// The ARCH3-R159 token: expires with the host.
  [[nodiscard]] std::weak_ptr<const void> alive_token() const noexcept { return alive_; }

private:
  [[nodiscard]] static result<void> seed(sl::fabric::workspace &ws, const host_spec &spec);
  [[nodiscard]] static result<sl::fabric::document_projection>
  head_projection(const sl::fabric::workspace &ws, const host_spec &spec);
  /// Takes the head projection and posts it to the link when `post`.
  [[nodiscard]] result<void> refresh(bool post);

  std::unique_ptr<sl::fabric::workspace> ws_;
  host_spec spec_;
  sl::fabric::document_projection projection_;
  link::world_link *link_ = nullptr;
  std::uint64_t commits_ = 0;
  std::optional<sl::fabric::commit_receipt> last_receipt_;
  std::shared_ptr<const int> alive_ = std::make_shared<const int>(0);
};

/// What link_rig builds.
struct rig_options {
  host_spec host;
  bool bind_body = true;
  bool bind_marker = false;
  bool bind_crate = false;
  /// Submit to link_rig::preview (a null_sink) instead of the host.
  bool preview_sink = false;
  std::uint32_t workers = 2;
};

/// The stack under a link. Members are destroyed in reverse order: the link first, the host last.
struct link_rig {
  std::unique_ptr<fabric_host> host;
  std::unique_ptr<sched_rig> stack;
  mem::tag_registry tags;
  mem::memory_tag projection_tag;
  std::unique_ptr<mem::diagnostics> diagnostics;
  type_registry types;
  ecs::component_id body_component;
  ecs::component_id marker_component;
  ecs::component_id crate_component;
  ecs::component_id spin_component;
  link::binding_registry bindings;
  link::null_sink preview;
  std::unique_ptr<link::world_link> link;

  /// The host, the stack (components registered and frozen), the bindings and the attached link;
  /// nothing ingested, nothing built.
  [[nodiscard]] static result<std::unique_ptr<link_rig>> make(const rig_options &options);
  /// Ingests the host's current projection.
  [[nodiscard]] result<link::link_stats> ingest() const;
  /// Builds the systems and the frames, then runs the priming frame (no tick).
  [[nodiscard]] result<void> build() const;
  /// Advances the clock one tick and runs one frame.
  [[nodiscard]] result<frame_stats> step() const;
  [[nodiscard]] result<ecs::entity> entity_of(const sl::fabric::record_id &record) const;
  /// The external bytes reported for fabric.projection.
  [[nodiscard]] std::uint64_t external_projection_bytes() const;
};

} // namespace sl::runtime::testing
