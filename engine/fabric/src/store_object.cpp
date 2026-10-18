// Object frames and batch checks (WO-042, SL-ARCH-001 ARCH1-R09 and R10, SL-ARCH-002 ARCH2-R02).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t header_size = 8; // magic, major, minor, kind, flags

std::string at_offset(std::string_view rule, std::size_t offset, std::string_view what) {
  std::string m(rule);
  m += " at offset ";
  m += std::to_string(offset);
  m += ": ";
  m += what;
  return m;
}

std::string truncated(std::size_t size) {
  return at_offset("R-SER-TRUNC", size, "frame ends early");
}

constexpr bool known_kind(std::uint8_t k) noexcept {
  return k >= static_cast<std::uint8_t>(object_kind::blob) &&
         k <= static_cast<std::uint8_t>(object_kind::schema_set);
}

bool valid_ref_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > max_ref_name) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b >= 0x21U && b <= 0x7EU;
  });
}

std::string indexed(std::string_view what, std::size_t i) {
  std::string m(what);
  m += ' ';
  m += std::to_string(i);
  return m;
}

} // namespace

object make_object(object_kind kind, byte_span payload) {
  const frame_header header{.kind = kind};
  return object{.header = header,
                .payload = std::vector<std::uint8_t>(payload.begin(), payload.end()),
                .id = hash_object(header, payload)};
}

result<object> parse_object(byte_span frame) noexcept {
  const std::size_t magic_seen = std::min(frame.size(), frame_magic.size());
  if (!std::ranges::equal(frame.first(magic_seen), byte_span{frame_magic}.first(magic_seen))) {
    return make_error(errc::object_corrupt, at_offset("R-SER-FRAME", 0, "bad magic"));
  }
  if (frame.size() < header_size) {
    return make_error(errc::decode_truncated, truncated(frame.size()));
  }
  const frame_header header{.major = frame[4],
                            .minor = frame[5],
                            .kind = static_cast<object_kind>(frame[6]),
                            .flags = frame[7]};
  if (header.major != frame_major) {
    return make_error(errc::format_unsupported,
                      at_offset("R-SER-FRAME", 4,
                                "major " + std::to_string(header.major) +
                                    ", this engine reads major " + std::to_string(frame_major)));
  }
  if (!known_kind(frame[6])) {
    return make_error(
        errc::object_corrupt,
        at_offset("R-SER-FRAME", 6, "unknown object kind " + std::to_string(frame[6])));
  }
  if (header.flags != 0) {
    return make_error(
        errc::object_corrupt,
        at_offset("R-SER-FRAME", 7, "reserved flags " + std::to_string(header.flags)));
  }
  byte_reader reader(frame.subspan(header_size));
  const result<std::uint64_t> length = reader.uvarint();
  if (!length) {
    if (length.error().code == errc::decode_truncated) {
      return make_error(errc::decode_truncated, truncated(frame.size()));
    }
    return make_error(errc::decode_noncanonical,
                      at_offset("R-SER-VARINT", header_size, "bad payload length varint"));
  }
  if (*length > max_object_payload) {
    return make_error(errc::decode_limit,
                      at_offset("R-SER-LIMIT", header_size, "payload length over 16 MiB"));
  }
  const std::size_t start = header_size + reader.offset();
  const auto size = static_cast<std::size_t>(*length);
  if (frame.size() - start < size) {
    return make_error(errc::decode_truncated, truncated(frame.size()));
  }
  if (frame.size() - start > size) {
    return make_error(errc::decode_noncanonical,
                      at_offset("R-SER-TRAILING", start + size,
                                std::to_string(frame.size() - start - size) + " trailing bytes"));
  }
  const byte_span payload = frame.subspan(start, size);
  return object{.header = header,
                .payload = std::vector<std::uint8_t>(payload.begin(), payload.end()),
                .id = hash_object(header, payload)};
}

std::vector<std::uint8_t> frame_object(const frame_header &header, byte_span payload) {
  byte_writer w;
  w.bytes(byte_span{frame_magic});
  w.u8(header.major);
  w.u8(header.minor);
  w.u8(static_cast<std::uint8_t>(header.kind));
  w.u8(header.flags);
  w.uvarint(payload.size());
  w.bytes(payload);
  return w.take();
}

std::vector<std::uint8_t> frame_object(object_kind kind, byte_span payload) {
  return frame_object(frame_header{.kind = kind}, payload);
}

result<void> check_writable(const frame_header &header) noexcept {
  if (header.major != frame_major) {
    return make_error(errc::format_unsupported,
                      "frame major " + std::to_string(header.major) + " is not writable");
  }
  if (header.minor > frame_minor) {
    return make_error(errc::read_only_newer, "frame minor " + std::to_string(header.minor) +
                                                 " is newer than " + std::to_string(frame_minor) +
                                                 ": readable, not writable");
  }
  if (!known_kind(static_cast<std::uint8_t>(header.kind))) {
    return make_error(errc::object_corrupt,
                      "unknown object kind " + std::to_string(static_cast<unsigned>(header.kind)));
  }
  if (header.flags != 0) {
    return make_error(errc::object_corrupt, "reserved frame flags " + std::to_string(header.flags));
  }
  return {};
}

result<void> check_batch(const commit_batch &batch) noexcept {
  if (batch.objects.empty() && batch.refs.empty() && batch.intents.empty()) {
    return make_error(errc::invalid_argument, "empty commit batch");
  }
  for (std::size_t i = 0; i < batch.objects.size(); ++i) {
    const object &o = batch.objects[i];
    if (result<void> w = check_writable(o.header); !w) {
      return make_error(w.error().code, indexed("object", i) + ": " + w.error().message);
    }
    if (o.payload.size() > max_object_payload) {
      return make_error(errc::decode_limit, indexed("object", i) + ": payload over 16 MiB");
    }
    const digest actual = hash_object(o.header, o.payload);
    if (actual != o.id) {
      return make_error(errc::object_corrupt, indexed("object", i) + ": id " + o.id.to_hex() +
                                                  " is not the hash of its frame " +
                                                  actual.to_hex());
    }
  }
  std::set<std::string_view> names;
  for (const ref_update &r : batch.refs) {
    if (!valid_ref_name(r.name)) {
      return make_error(errc::invalid_argument,
                        "bad ref name '" + r.name + "' (1 to 255 bytes of 0x21..0x7E)");
    }
    if (!names.insert(r.name).second) {
      return make_error(errc::invalid_argument, "ref '" + r.name + "' updated twice in one batch");
    }
  }
  for (std::size_t i = 0; i < batch.intents.size(); ++i) {
    const intent_record &in = batch.intents[i];
    if (in.operation.is_null()) {
      return make_error(errc::invalid_argument, indexed("intent", i) + ": null operation id");
    }
    const bool begin = in.phase == intent_phase::begin;
    if (begin && (in.kind.empty() || in.outcome != intent_outcome::none)) {
      return make_error(errc::invalid_argument,
                        indexed("intent", i) + ": a begin needs a kind and no outcome");
    }
    if (!begin && (in.phase != intent_phase::end || in.outcome == intent_outcome::none)) {
      return make_error(errc::invalid_argument, indexed("intent", i) + ": an end needs an outcome");
    }
  }
  return {};
}

} // namespace sl::fabric
