// The in-memory store backend (WO-042, SL-ARCH-002 ARCH2-R02 and R07, ADR-0014).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric {

struct memory_backend::state {
  std::shared_ptr<memory_backend_faults> faults;
  std::shared_mutex mutex;
  std::map<digest, std::vector<std::uint8_t>> objects; // id -> SLFO frame
  std::map<std::string, digest, std::less<>> refs;
  std::vector<intent_record> intents;
  std::set<record_id> open;
  std::uint64_t seq = 0;
  std::int64_t last_commit_time_us = 0;
};

namespace {

// What a commit has applied so far, so that a failed step can put everything back.
struct undo_log {
  std::vector<digest> objects;
  std::vector<std::pair<std::string, std::optional<digest>>> refs;
  std::size_t intents = 0;
};

std::string ref_text(std::string_view name) {
  std::string m("ref '");
  m += name;
  m += '\'';
  return m;
}

std::string indexed(std::string_view what, std::size_t i) {
  std::string m(what);
  m += ' ';
  m += std::to_string(i);
  return m;
}

} // namespace

memory_backend::memory_backend(std::shared_ptr<memory_backend_faults> faults)
    : state_(std::make_unique<state>()) {
  state_->faults = std::move(faults);
}

memory_backend::~memory_backend() = default;

result<object> memory_backend::get(const digest &id) const noexcept {
  std::vector<std::uint8_t> frame;
  {
    const std::shared_lock lock(state_->mutex);
    const auto it = state_->objects.find(id);
    if (it == state_->objects.end()) {
      return make_error(errc::object_missing, "object " + id.to_hex() + " is not stored");
    }
    frame = it->second;
  }
  if (state_->faults) {
    state_->faults->on_read(id, frame);
  }
  result<object> parsed = parse_object(frame);
  if (!parsed) {
    return make_error(errc::object_corrupt,
                      "object " + id.to_hex() +
                          ": stored frame does not parse: " + parsed.error().message);
  }
  if (parsed->id != id) {
    return make_error(errc::object_corrupt,
                      "object " + id.to_hex() + ": stored bytes hash to " + parsed->id.to_hex());
  }
  return parsed;
}

result<bool> memory_backend::has(const digest &id) const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->objects.contains(id);
}

result<digest> memory_backend::read_ref(std::string_view name) const noexcept {
  const std::shared_lock lock(state_->mutex);
  const auto it = state_->refs.find(name);
  if (it == state_->refs.end()) {
    return make_error(errc::not_found, "no " + ref_text(name));
  }
  return it->second;
}

result<std::vector<ref_entry>> memory_backend::list_refs() const noexcept {
  const std::shared_lock lock(state_->mutex);
  std::vector<ref_entry> out;
  out.reserve(state_->refs.size());
  for (const auto &[name, target] : state_->refs) {
    out.push_back(ref_entry{.name = name, .target = target});
  }
  return out;
}

result<commit_ack> memory_backend::commit(const mutation_capability & /*cap*/,
                                          commit_batch batch) noexcept {
  // F5 ledger (WO-051): every call is a backend mutation; outside the manager it is a bypass.
  f5::note_mutation("memory_backend::commit", this);
  if (!batch.intents.empty()) {
    f5::note_mutation("memory_backend::commit intents", this);
  }
  if (result<void> checked = check_batch(batch); !checked) {
    return std::unexpected(checked.error());
  }
  const std::unique_lock lock(state_->mutex);
  state &s = *state_;

  // Everything that can refuse the batch runs before the first write.
  std::set<digest> incoming;
  for (const object &o : batch.objects) {
    incoming.insert(o.id);
  }
  for (const ref_update &r : batch.refs) {
    const auto it = s.refs.find(r.name);
    if (r.expected) {
      if (it == s.refs.end()) {
        return make_error(errc::ref_cas_failed,
                          ref_text(r.name) + " does not exist, expected " + r.expected->to_hex());
      }
      if (it->second != *r.expected) {
        return make_error(errc::ref_cas_failed, ref_text(r.name) + " is at " + it->second.to_hex() +
                                                    ", expected " + r.expected->to_hex());
      }
    } else if (it != s.refs.end()) {
      return make_error(errc::ref_cas_failed, ref_text(r.name) + " already exists at " +
                                                  it->second.to_hex() + " (create-if-absent)");
    }
    if (!incoming.contains(r.desired) && !s.objects.contains(r.desired)) {
      return make_error(errc::object_missing, ref_text(r.name) + " target " + r.desired.to_hex() +
                                                  " is neither stored nor in the batch");
    }
  }
  std::set<record_id> open = s.open;
  for (const intent_record &in : batch.intents) {
    if (in.phase == intent_phase::begin && !open.insert(in.operation).second) {
      return make_error(errc::invalid_argument,
                        "intent " + in.operation.to_string() + " is already open");
    }
    if (in.phase == intent_phase::end && open.erase(in.operation) == 0) {
      return make_error(errc::invalid_argument,
                        "intent " + in.operation.to_string() + " ends without a begin");
    }
  }

  // Apply step by step; a failed step undoes every step before it.
  undo_log undo{.objects = {}, .refs = {}, .intents = s.intents.size()};
  const auto rollback = [&s, &undo] {
    s.intents.resize(undo.intents);
    for (const auto &[name, prior] : std::views::reverse(undo.refs)) {
      if (prior) {
        s.refs.insert_or_assign(name, *prior);
      } else {
        s.refs.erase(name);
      }
    }
    for (const digest &id : undo.objects) {
      s.objects.erase(id);
    }
  };
  const auto step = [&s](std::string_view name) -> result<void> {
    if (s.faults) {
      return s.faults->before_step(name);
    }
    return {};
  };

  for (std::size_t i = 0; i < batch.objects.size(); ++i) {
    if (result<void> ok = step(indexed("object", i)); !ok) {
      rollback();
      return std::unexpected(ok.error());
    }
    const object &o = batch.objects[i];
    if (s.objects.contains(o.id)) {
      continue; // already stored: idempotent
    }
    s.objects.emplace(o.id, frame_object(o.header, o.payload));
    undo.objects.push_back(o.id);
  }
  for (ref_update &r : batch.refs) {
    if (result<void> ok = step("ref " + r.name); !ok) {
      rollback();
      return std::unexpected(ok.error());
    }
    const auto it = s.refs.find(r.name);
    undo.refs.emplace_back(r.name,
                           it == s.refs.end() ? std::nullopt : std::optional<digest>(it->second));
    s.refs.insert_or_assign(std::move(r.name), r.desired);
  }
  for (std::size_t i = 0; i < batch.intents.size(); ++i) {
    if (result<void> ok = step(indexed("intent", i)); !ok) {
      rollback();
      return std::unexpected(ok.error());
    }
    s.intents.push_back(std::move(batch.intents[i]));
  }
  if (result<void> ok = step("seq"); !ok) {
    rollback();
    return std::unexpected(ok.error());
  }
  s.open = std::move(open);
  s.last_commit_time_us = batch.commit_time_us;
  return commit_ack{.seq = ++s.seq};
}

result<verify_report> memory_backend::verify(const verify_scope &scope) const noexcept {
  std::vector<digest> stored;
  {
    const std::shared_lock lock(state_->mutex);
    stored.reserve(state_->objects.size());
    for (const auto &entry : state_->objects) {
      stored.push_back(entry.first);
    }
  }
  return verify_store(*this, scope, stored);
}

std::size_t memory_backend::object_count() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->objects.size();
}

std::uint64_t memory_backend::last_seq() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->seq;
}

std::vector<intent_record> memory_backend::intents() const {
  const std::shared_lock lock(state_->mutex);
  return state_->intents;
}

std::vector<record_id> memory_backend::open_intents() const {
  const std::shared_lock lock(state_->mutex);
  return {state_->open.begin(), state_->open.end()};
}

} // namespace sl::fabric
