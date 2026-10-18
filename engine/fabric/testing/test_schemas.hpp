#pragma once

// A small scene/entity/component module set for the fabric tests (WO-039; used by WO-041 to
// WO-062). It is test data only: the real Fabric types come from schemas/fabric (ARCH1-R19).
//
//   content.scene.Meters         tagged f64
//   content.scene.Vec3           struct {1 x f64, 2 y f64, 3 z f64}
//   content.scene.Layer          enum {0 world, 1 ui, 2 debug}
//   content.scene.Transform      struct {1 position Vec3, 2 scale f64 optional default 1.0,
//                                        3 radius Meters optional}
//   content.scene.MeshRenderer   struct {1 mesh ref strong -> Mesh, 2 cast_shadows bool optional
//                                        default true}
//   content.scene.Tag            struct {1 label string}
//   content.scene.Component      variant {1 transform, 2 mesh_renderer, 3 tag}
//   content.scene.Components     oset of Component
//   content.scene.EntityList     oset of ref
//   content.scene.Properties     map string -> string
//   content.scene.Lods           list of f32
//   content.scene.Entity         record (entity, document, domain content.scene) struct
//                                {1 name string, 2 layer Layer optional default 0, 3 parent ref
//                                weak
//                                 -> Entity optional, 4 components Components, 5 properties
//                                 Properties optional, 6 lods Lods optional; reserved 7}
//   content.scene.Scene          record (entity, document, document root, domain content.scene)
//                                struct {1 name string, 2 entities EntityList owned -> Entity,
//                                3 gravity Vec3 optional}
//   content.mesh.Mesh            record (asset, document, document root, domain content.mesh)
//                                struct {1 source external_ref, 2 vertex_count u32,
//                                3 content blob_ref optional}
//   asset.cooked.CookedMesh      record (derived, derived, domain asset.cooked) struct
//                                {1 source ref strong -> Mesh, 2 payload blob_ref}

#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

namespace test_types {
inline constexpr std::string_view meters = "content.scene.Meters";
inline constexpr std::string_view vec3 = "content.scene.Vec3";
inline constexpr std::string_view layer = "content.scene.Layer";
inline constexpr std::string_view transform = "content.scene.Transform";
inline constexpr std::string_view mesh_renderer = "content.scene.MeshRenderer";
inline constexpr std::string_view tag = "content.scene.Tag";
inline constexpr std::string_view component = "content.scene.Component";
inline constexpr std::string_view components = "content.scene.Components";
inline constexpr std::string_view entity_list = "content.scene.EntityList";
inline constexpr std::string_view properties = "content.scene.Properties";
inline constexpr std::string_view lods = "content.scene.Lods";
inline constexpr std::string_view entity = "content.scene.Entity";
inline constexpr std::string_view scene = "content.scene.Scene";
inline constexpr std::string_view mesh = "content.mesh.Mesh";
inline constexpr std::string_view cooked_mesh = "asset.cooked.CookedMesh";
} // namespace test_types

/// Adds every type above to the unfrozen `reg`.
[[nodiscard]] result<void> add_test_schemas(schema_registry &reg);

/// A frozen registry holding the built-ins and the test schemas.
[[nodiscard]] result<schema_registry> make_test_registry();

/// derive(c, well_known(namespace, 1), key): stable ids for fixtures.
[[nodiscard]] record_id test_id(id_class c, std::string_view key) noexcept;

/// A Vec3 value.
[[nodiscard]] result<value> make_vec3(double x, double y, double z);

/// An Entity named `name` with a transform and a tag component (elements test_id(element,
/// "<name>/transform") and "<name>/tag"), a properties map and no parent; it conforms.
[[nodiscard]] result<value> sample_entity(std::string_view name);

/// A Scene named `name` owning the entities test_id(entity, "<name>/e<i>") for i < count.
[[nodiscard]] result<value> sample_scene(std::string_view name, unsigned count);

} // namespace sl::fabric::testing
