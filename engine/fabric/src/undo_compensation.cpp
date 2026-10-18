// Compensations, compensators and the in-memory intent log of the undo manager (WO-052,
// SL-ARCH-002 ARCH2-R24; ADR-0018). The protocol is described in sl/fabric/undo.hpp.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint32_t command_field = 1;
constexpr std::uint32_t args_field = 2;

} // namespace

result<patch_op> op_compensation(const compensation &c) {
  if (c.command.empty()) {
    return make_error(errc::invalid_argument, "a compensation needs a command");
  }
  auto command = value::string(c.command);
  if (!command) {
    return std::unexpected(command.error());
  }
  std::vector<field> fields;
  fields.push_back(field{.no = command_field, .v = std::move(*command)});
  fields.push_back(field{.no = args_field, .v = c.args});
  auto note = value::struct_(std::move(fields));
  if (!note) {
    return std::unexpected(note.error());
  }
  patch_op op = op_annotate(std::move(*note));
  op.rev = reversibility::compensable;
  return op;
}

result<std::optional<compensation>> compensation_of(const patch_op &op) {
  if (op.kind != op_kind::annotate || op.rev != reversibility::compensable) {
    return std::optional<compensation>{};
  }
  if (!op.new_value || op.new_value->kind() != value_kind::struct_) {
    return make_error(errc::type_mismatch, "a compensation note is not a struct");
  }
  const value *command = op.new_value->find_field(command_field);
  const value *args = op.new_value->find_field(args_field);
  if (command == nullptr || command->kind() != value_kind::string || command->as_string().empty()) {
    return make_error(errc::type_mismatch, "a compensation note has no command (field 1)");
  }
  if (args == nullptr) {
    return make_error(errc::type_mismatch, "a compensation note has no args (field 2)");
  }
  return std::optional<compensation>{
      compensation{.command = std::string(command->as_string()), .args = *args}};
}

result<void> compensator_registry::add(std::shared_ptr<const compensator> c) {
  if (!c) {
    return make_error(errc::invalid_argument, "null compensator");
  }
  const std::string command(c->command());
  if (command.empty()) {
    return make_error(errc::invalid_argument, "a compensator needs a command");
  }
  if (by_command_.contains(command)) {
    return make_error(errc::already_exists, "compensator '" + command + "' is registered");
  }
  by_command_.emplace(command, std::move(c));
  return {};
}

const compensator *compensator_registry::find(std::string_view command) const noexcept {
  const auto it = by_command_.find(command);
  return it == by_command_.end() ? nullptr : it->second.get();
}

result<void> memory_intent_log::write(const intent_record &r) {
  if (r.operation.is_null()) {
    return make_error(errc::invalid_argument, "an intent needs an operation id");
  }
  const std::scoped_lock lock(mutex_);
  records_.push_back(r);
  return {};
}

std::vector<intent_record> memory_intent_log::records() const {
  const std::scoped_lock lock(mutex_);
  return records_;
}

std::vector<record_id> memory_intent_log::open() const {
  const std::scoped_lock lock(mutex_);
  std::set<record_id> open;
  for (const intent_record &r : records_) {
    if (r.phase == intent_phase::begin) {
      open.insert(r.operation);
    } else {
      open.erase(r.operation);
    }
  }
  return {open.begin(), open.end()};
}

} // namespace sl::fabric
