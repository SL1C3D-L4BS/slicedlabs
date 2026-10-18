// Event envelopes: correlation mapping, derived ids, validation, sources, the JSON Lines form and
// the sl::log bridge (WO-047, SL-ARCH-001 ARCH1-R46 to ARCH1-R49).

#include "sl/fabric/events.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t max_type_size = 255;

std::unexpected<error> invalid(std::string message) {
  return make_error(errc::event_invalid, std::move(message));
}

bool printable_ascii(std::string_view s) noexcept {
  return std::ranges::all_of(s, [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x21U && u <= 0x7EU;
  });
}

bool has_class(const record_id &id, id_class c) noexcept { return !id.is_null() && id.cls() == c; }

void append_key(std::string &out, std::string_view key) {
  sl::log::append_json_string(out, key);
  out += ':';
}

void append_id(std::string &out, std::string_view key, const record_id &id) {
  append_key(out, key);
  sl::log::append_json_string(out, id.to_string());
}

} // namespace

sl::log::correlation to_log(const correlation_record &c) noexcept {
  return sl::log::correlation{.id = c.id, .scope = std::string_view{c.scope}, .tick = c.tick};
}

correlation_record from_log(const sl::log::correlation &c) {
  return correlation_record{.id = c.id, .scope = std::string{c.scope}, .tick = c.tick};
}

correlation_record current_correlation_record() { return from_log(sl::log::current_correlation()); }

record_id event_id_for(const record_id &parent, std::string_view kind, std::uint64_t seq) {
  std::string key{kind};
  key += ':';
  key += std::to_string(seq);
  return derive(id_class::event, parent, key);
}

result<void> validate(const event_envelope &e) {
  if (!has_class(e.id, id_class::event)) {
    return invalid("id: expected an event id, got " + e.id.to_string());
  }
  if (e.type.empty() || e.type.size() > max_type_size || !printable_ascii(e.type)) {
    return invalid("type: expected 1 to 255 printable ASCII characters");
  }
  if (!has_class(e.schema.type, id_class::type)) {
    return invalid("schema.type: expected a type id, got " + e.schema.type.to_string());
  }
  if (e.seq == 0) {
    return invalid("seq: expected a sequence number of at least 1");
  }
  if (!has_class(e.actor, id_class::actor)) {
    return invalid("actor: expected an actor id, got " + e.actor.to_string());
  }
  if (const auto *subject = std::get_if<record_id>(&e.about.alt)) {
    if (subject->is_null()) {
      return invalid("about: a subject id must not be null");
    }
  } else if (std::get<std::string>(e.about.alt).empty()) {
    return invalid("about: exactly one of subject id or scope tag; the scope tag is empty");
  }
  if (e.correlation.scope.empty()) {
    return invalid("correlation.scope: F6 requires a scope tag, it is empty");
  }
  if (e.causality.cause && !has_class(*e.causality.cause, id_class::event)) {
    return invalid("causality.cause: expected an event id");
  }
  if (e.causality.tx && !has_class(*e.causality.tx, id_class::tx)) {
    return invalid("causality.tx: expected a tx id");
  }
  return {};
}

result<event_envelope> event_source::make(event_spec spec) {
  event_envelope e;
  e.schema = spec.schema;
  e.type = std::move(spec.type);
  e.time = clock_->now_us();
  e.seq = next_seq_;
  e.actor = spec.actor;
  e.about = std::move(spec.about);
  e.correlation = spec.correlation ? std::move(*spec.correlation) : current_correlation_record();
  e.causality = spec.causality;
  if (spec.payload) {
    auto bytes = encode(*spec.payload);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    e.payload = std::move(*bytes);
  }
  const record_id &parent = e.causality.tx ? *e.causality.tx : source_;
  if (parent.is_null()) {
    return invalid("id: the source id and the transaction are both null");
  }
  e.id = event_id_for(parent, e.type, e.seq);
  if (auto ok = validate(e); !ok) {
    return std::unexpected(ok.error());
  }
  ++next_seq_;
  return e;
}

result<std::string> to_json_line(const event_envelope &e) {
  if (auto ok = validate(e); !ok) {
    return std::unexpected(ok.error());
  }
  std::string out;
  out += '{';
  append_key(out, "format");
  sl::log::append_json_string(out, event_json_format);
  out += ',';
  append_id(out, "id", e.id);
  out += ",\"schema\":{";
  append_id(out, "type", e.schema.type);
  out += ",\"major\":" + std::to_string(e.schema.major);
  out += ",\"minor\":" + std::to_string(e.schema.minor) + '}';
  out += ',';
  append_key(out, "type");
  sl::log::append_json_string(out, e.type);
  out += ",\"time_us\":" + std::to_string(e.time);
  out += ",\"seq\":" + std::to_string(e.seq);
  out += ',';
  append_id(out, "actor", e.actor);
  out += ',';
  if (const auto *subject = std::get_if<record_id>(&e.about.alt)) {
    append_id(out, "subject", *subject);
  } else {
    append_key(out, "scope");
    sl::log::append_json_string(out, std::get<std::string>(e.about.alt));
  }
  out += R"(,"correlation":{"id":)" + std::to_string(e.correlation.id) + ',';
  append_key(out, "scope");
  sl::log::append_json_string(out, e.correlation.scope);
  out += ",\"tick\":" + std::to_string(e.correlation.tick) + '}';
  out += ",\"causality\":{";
  const char *sep = "";
  if (e.causality.cause) {
    append_id(out, "cause", *e.causality.cause);
    sep = ",";
  }
  if (e.causality.tx) {
    out += sep;
    append_id(out, "tx", *e.causality.tx);
    sep = ",";
  }
  if (e.causality.revision) {
    auto text = digest_text(*e.causality.revision);
    if (!text) {
      return std::unexpected(text.error());
    }
    out += sep;
    append_key(out, "revision");
    sl::log::append_json_string(out, *text);
  }
  out += '}';
  if (e.payload) {
    out += ',';
    append_key(out, "payload");
    sl::log::append_json_string(out, to_hex(*e.payload));
  }
  out += '}';
  return out;
}

void log_event(sl::log::logger &target, const event_envelope &e, sl::log::level lvl) {
  const std::string id = e.id.to_string();
  std::string tx;
  std::string subject;
  std::vector<sl::log::field> fields;
  fields.reserve(4);
  fields.emplace_back("event_id", id);
  fields.emplace_back("seq", e.seq);
  if (const auto *s = std::get_if<record_id>(&e.about.alt)) {
    subject = s->to_string();
    fields.emplace_back("subject", subject);
  } else {
    fields.emplace_back("scope", std::string_view{std::get<std::string>(e.about.alt)});
  }
  if (e.causality.tx) {
    tx = e.causality.tx->to_string();
    fields.emplace_back("tx", tx);
  }
  const auto when = std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::microseconds{e.time})};
  target.log(sl::log::event{.level = lvl,
                            .system = "fabric.event",
                            .message = e.type,
                            .fields = fields,
                            .correlation = to_log(e.correlation),
                            .where = std::source_location::current(),
                            .ts = when});
}

} // namespace sl::fabric
