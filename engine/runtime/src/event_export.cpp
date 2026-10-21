// event_exporter (WO-092, SL-ARCH-003 ARCH3-R142, ARCH1-R46, master plan section 3 F6): the events
// a frame delivered become fabric.event envelopes on the runtime's own bus, stamped by its one
// event_source, so their seq is strictly increasing. Per event: type "runtime.event.<name>", the
// payload schema ref derive(type, source, type string) at 1.0, the actor derive(actor, source,
// "runtime.events"), the scope tag "runtime", the event's correlation {id, scope, tick} (scope
// "runtime" when empty) and the reflected bytes of every flagged field as one SLB/1 bytes value.
// The whole frame is one publish_batch: nothing is published when an envelope is refused.

#include "sl/runtime/events.hpp"

#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

bool by_type(const event_stream *a, const event_stream *b) noexcept {
  return a->type() < b->type();
}

} // namespace

event_exporter::event_exporter(sl::fabric::event_bus &bus,
                               sl::fabric::event_source &source) noexcept
    : bus_(&bus), source_(&source), owner_(std::this_thread::get_id()) {}

result<void> event_exporter::export_frame(std::uint64_t frame,
                                          std::span<const event_stream *const> channels) noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "event_exporter::export_frame off the owner thread");
  }
  std::vector<const event_stream *> ordered(channels.begin(), channels.end());
  std::ranges::sort(ordered, &by_type);
  const sl::fabric::record_id &source_id = source_->source();
  const sl::fabric::record_id actor =
      sl::fabric::derive(sl::fabric::id_class::actor, source_id, "runtime.events");

  std::vector<sl::fabric::event_envelope> batch;
  std::vector<event_record> records;
  for (const event_stream *s : ordered) {
    records.clear();
    s->collect(frame, records);
    const std::string type = "runtime.event." + std::string(s->name());
    const sl::fabric::record_id schema =
        sl::fabric::derive(sl::fabric::id_class::type, source_id, type);
    for (const event_record &r : records) {
      std::vector<std::uint8_t> raw(r.payload.size());
      std::ranges::transform(r.payload, raw.begin(),
                             [](std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); });
      auto payload = sl::fabric::value::bytes(raw);
      if (!payload) {
        return make_error(errc::internal, "event_exporter: the payload does not encode");
      }
      sl::fabric::event_spec spec;
      spec.type = type;
      spec.schema = sl::fabric::event_schema_ref{.type = schema, .major = 1, .minor = 0};
      spec.actor = actor;
      spec.about = sl::fabric::event_about{.alt = std::string{"runtime"}};
      sl::fabric::correlation_record corr = sl::fabric::from_log(r.envelope.correlation);
      if (corr.scope.empty()) {
        corr.scope = "runtime";
      }
      spec.correlation = std::move(corr);
      spec.payload = std::move(*payload);
      auto made = source_->make(std::move(spec));
      if (!made) {
        return make_error(errc::internal, "event_exporter: the event source refused an envelope");
      }
      batch.push_back(std::move(*made));
    }
  }
  if (batch.empty()) {
    return {};
  }
  const std::size_t n = batch.size();
  if (auto published = bus_->publish_batch(std::move(batch)); !published) {
    return make_error(errc::internal, "event_exporter: the bus refused the frame's envelopes");
  }
  exported_ += n;
  return {};
}

} // namespace sl::runtime
