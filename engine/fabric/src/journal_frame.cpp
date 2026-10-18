// Segment headers and frames of the commit log (WO-053, SL-ARCH-002 `journal`, ARCH2-R29 and
// R31, ADR-0015).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t segment_crc_at = 20;
constexpr std::string_view segment_prefix = "seg-";
constexpr std::string_view segment_suffix = ".slg";
constexpr std::size_t segment_digits = 8;

std::uint32_t load_u32le(byte_span b) noexcept {
  return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8U) |
         (static_cast<std::uint32_t>(b[2]) << 16U) | (static_cast<std::uint32_t>(b[3]) << 24U);
}

std::uint64_t load_u64le(byte_span b) noexcept {
  return static_cast<std::uint64_t>(load_u32le(b.first(4))) |
         (static_cast<std::uint64_t>(load_u32le(b.subspan(4, 4))) << 32U);
}

void store_u32le(std::span<std::uint8_t> out, std::uint32_t v) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>(v >> (8U * i));
  }
}

void store_u64le(std::span<std::uint8_t> out, std::uint64_t v) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>(v >> (8U * i));
  }
}

struct frame_prefix {
  std::uint32_t len = 0;
  std::uint32_t crc = 0;
  std::uint8_t type = 0;
};

frame_prefix read_prefix(byte_span b) noexcept {
  return frame_prefix{
      .len = load_u32le(b.first(4)), .crc = load_u32le(b.subspan(4, 4)), .type = b[8]};
}

std::string at_offset(std::string_view reason, std::uint64_t offset, std::string_view detail) {
  std::string m(reason);
  m += " at offset ";
  m += std::to_string(offset);
  if (!detail.empty()) {
    m += " (";
    m += detail;
    m += ')';
  }
  return m;
}

// The checks on a prefix that need no payload: length bounds against what is left.
frame_fault check_bounds(const frame_prefix &p, std::uint64_t remaining) noexcept {
  if (p.len == 0 || p.len > max_frame_len) {
    return frame_fault::bad_length;
  }
  if (remaining - 8 < p.len) {
    return frame_fault::truncated_payload;
  }
  return frame_fault::none;
}

std::string bounds_detail(const frame_prefix &p, std::uint64_t remaining) {
  return "len " + std::to_string(p.len) + ", " + std::to_string(remaining) + " bytes left";
}

void set_stop(frame_stop &stop, frame_fault fault, std::uint64_t offset, std::string_view detail) {
  stop.fault = fault;
  stop.offset = offset;
  stop.message = at_offset(to_string(fault), offset, detail);
}

} // namespace

std::string segment_file_name(std::uint32_t number) {
  std::string digits = std::to_string(number);
  std::string name(segment_prefix);
  if (digits.size() < segment_digits) {
    name.append(segment_digits - digits.size(), '0');
  }
  name += digits;
  name += segment_suffix;
  return name;
}

std::optional<std::uint32_t> parse_segment_file_name(std::string_view name) noexcept {
  if (name.size() != segment_prefix.size() + segment_digits + segment_suffix.size() ||
      !name.starts_with(segment_prefix) || !name.ends_with(segment_suffix)) {
    return std::nullopt;
  }
  std::uint32_t n = 0;
  for (const char c : name.substr(segment_prefix.size(), segment_digits)) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    n = (n * 10U) + static_cast<std::uint32_t>(c - '0');
  }
  if (n == 0) {
    return std::nullopt;
  }
  return n;
}

std::array<std::uint8_t, segment_header_size>
encode_segment_header(const segment_header &h) noexcept {
  std::array<std::uint8_t, segment_header_size> out{};
  std::ranges::copy(segment_magic, out.begin());
  out[4] = h.major;
  out[5] = h.minor;
  out[6] = 0;
  out[7] = 0;
  store_u32le(std::span(out).subspan(8, 4), h.segment);
  store_u64le(std::span(out).subspan(12, 8), h.first_seq);
  store_u32le(std::span(out).subspan(segment_crc_at, 4),
              crc32c(std::span(out).first(segment_crc_at)));
  return out;
}

result<segment_header> parse_segment_header(byte_span bytes) noexcept {
  if (bytes.size() < segment_header_size) {
    return make_error(errc::decode_truncated, "segment header: " + std::to_string(bytes.size()) +
                                                  " bytes, need " +
                                                  std::to_string(segment_header_size));
  }
  if (!std::ranges::equal(bytes.first(segment_magic.size()), segment_magic)) {
    return make_error(errc::journal_corrupt, "segment header: bad magic at offset 0");
  }
  segment_header h{.major = bytes[4],
                   .minor = bytes[5],
                   .segment = load_u32le(bytes.subspan(8, 4)),
                   .first_seq = load_u64le(bytes.subspan(12, 8))};
  if (h.major != journal_major) {
    return make_error(errc::format_unsupported, "segment header: major " + std::to_string(h.major) +
                                                    ", supported " + std::to_string(journal_major));
  }
  const std::uint32_t want = crc32c(bytes.first(segment_crc_at));
  if (load_u32le(bytes.subspan(segment_crc_at, 4)) != want) {
    return make_error(errc::journal_corrupt, "segment header: crc mismatch at offset 20");
  }
  if (bytes[6] != 0 || bytes[7] != 0) {
    return make_error(errc::journal_corrupt, "segment header: reserved bytes are not zero");
  }
  return h;
}

result<std::vector<std::uint8_t>> encode_frame(std::uint8_t type, byte_span payload) {
  if (payload.size() >= max_frame_len) {
    return make_error(errc::encode_invalid, "frame payload of " + std::to_string(payload.size()) +
                                                " bytes exceeds the 64 MiB frame cap");
  }
  const auto len = static_cast<std::uint32_t>(payload.size() + 1);
  const std::array<std::uint8_t, 1> type_byte{type};
  const std::uint32_t crc = crc32c_extend(crc32c(type_byte), payload);
  std::vector<std::uint8_t> out(frame_prefix_size + payload.size());
  store_u32le(std::span(out).first(4), len);
  store_u32le(std::span(out).subspan(4, 4), crc);
  out[8] = type;
  std::ranges::copy(payload, out.begin() + frame_prefix_size);
  return out;
}

std::string_view to_string(frame_fault f) noexcept {
  switch (f) {
  case frame_fault::none:
    return "clean end";
  case frame_fault::truncated_prefix:
    return "truncated frame header";
  case frame_fault::bad_length:
    return "frame length out of bounds";
  case frame_fault::truncated_payload:
    return "truncated frame payload";
  case frame_fault::crc_mismatch:
    return "frame crc mismatch";
  case frame_fault::unknown_type:
    return "unknown frame type";
  }
  return "unknown fault";
}

frame_reader::frame_reader(byte_span data, std::uint64_t base) noexcept : data_(data), base_(base) {
  stop_.offset = base;
  stop_.end = base;
}

std::optional<frame_view> frame_reader::next() {
  while (!done_) {
    const std::uint64_t offset = base_ + pos_;
    const std::size_t remaining = data_.size() - pos_;
    if (remaining == 0) {
      stop_.offset = offset;
      done_ = true;
      return std::nullopt;
    }
    if (remaining < frame_prefix_size) {
      set_stop(stop_, frame_fault::truncated_prefix, offset,
               std::to_string(remaining) + " bytes left");
      done_ = true;
      return std::nullopt;
    }
    const frame_prefix p = read_prefix(data_.subspan(pos_, frame_prefix_size));
    if (const frame_fault f = check_bounds(p, remaining); f != frame_fault::none) {
      set_stop(stop_, f, offset, bounds_detail(p, remaining));
      done_ = true;
      return std::nullopt;
    }
    const byte_span covered = data_.subspan(pos_ + 8, p.len); // type || payload
    if (crc32c(covered) != p.crc) {
      set_stop(stop_, frame_fault::crc_mismatch, offset, {});
      done_ = true;
      return std::nullopt;
    }
    const frame_type_class cls = classify_frame_type(p.type);
    if (cls == frame_type_class::corrupt) {
      set_stop(stop_, frame_fault::unknown_type, offset, "type " + std::to_string(p.type));
      done_ = true;
      return std::nullopt;
    }
    pos_ += 8 + static_cast<std::size_t>(p.len);
    stop_.end = base_ + pos_;
    if (cls == frame_type_class::skipped) {
      ++stop_.skipped;
      continue;
    }
    return frame_view{
        .offset = offset, .type = static_cast<frame_type>(p.type), .payload = covered.subspan(1)};
  }
  return std::nullopt;
}

result<frame_stop> scan_segment(const vfs_file &file, std::uint64_t start,
                                const frame_visitor &visit) {
  const result<std::uint64_t> size = file.size();
  if (!size) {
    return std::unexpected(size.error());
  }
  frame_stop stop;
  stop.offset = start;
  stop.end = start;
  std::array<std::uint8_t, frame_prefix_size> head{};
  std::vector<std::uint8_t> payload;
  std::uint64_t pos = start;
  while (true) {
    if (pos >= *size) {
      stop.offset = pos;
      return stop;
    }
    const std::uint64_t remaining = *size - pos;
    if (remaining < frame_prefix_size) {
      set_stop(stop, frame_fault::truncated_prefix, pos, std::to_string(remaining) + " bytes left");
      return stop;
    }
    if (result<void> ok = file.read_exact(pos, head); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    const frame_prefix p = read_prefix(head);
    if (const frame_fault f = check_bounds(p, remaining); f != frame_fault::none) {
      set_stop(stop, f, pos, bounds_detail(p, remaining));
      return stop;
    }
    payload.resize(p.len - 1U);
    if (result<void> ok = file.read_exact(pos + frame_prefix_size, payload); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    const std::array<std::uint8_t, 1> type_byte{p.type};
    if (crc32c_extend(crc32c(type_byte), payload) != p.crc) {
      set_stop(stop, frame_fault::crc_mismatch, pos, {});
      return stop;
    }
    const frame_type_class cls = classify_frame_type(p.type);
    if (cls == frame_type_class::corrupt) {
      set_stop(stop, frame_fault::unknown_type, pos, "type " + std::to_string(p.type));
      return stop;
    }
    if (cls == frame_type_class::skipped) {
      ++stop.skipped;
    } else {
      const frame_view view{
          .offset = pos, .type = static_cast<frame_type>(p.type), .payload = payload};
      if (result<void> ok = visit(view); !ok) {
        return std::unexpected(std::move(ok.error()));
      }
    }
    pos += 8 + static_cast<std::uint64_t>(p.len);
    stop.end = pos;
  }
}

} // namespace sl::fabric
