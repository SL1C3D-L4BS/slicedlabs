// entity_map errors (WO-083, SL-ARCH-003 ARCH3-R105, ADR-0025): the non-template part of
// ecs::entity_map<Key>, so every instantiation shares one copy of each message.

#include "sl/runtime/ecs/entity.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"

namespace sl::runtime::ecs::detail {

std::unexpected<error> entity_map_null_entity() {
  return make_error(errc::invalid_argument, "entity_map: the null entity cannot be mapped");
}

std::unexpected<error> entity_map_duplicate_key() {
  return make_error(errc::already_exists, "entity_map: the key is already mapped");
}

std::unexpected<error> entity_map_duplicate_entity() {
  return make_error(errc::already_exists, "entity_map: the entity is already mapped");
}

std::unexpected<error> entity_map_key_not_found() {
  return make_error(errc::not_found, "entity_map: the key is not mapped");
}

std::unexpected<error> entity_map_entity_not_found() {
  return make_error(errc::not_found, "entity_map: the entity is not mapped");
}

} // namespace sl::runtime::ecs::detail
