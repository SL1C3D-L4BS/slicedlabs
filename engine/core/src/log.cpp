#include "sl/core/log.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <system_error>
#include <type_traits>
#include <utility>

namespace sl::log {

namespace {

// The process-wide correlation state, behind one accessor instead of mutable globals.
struct correlation_state {
  std::mutex scope_mutex;
  std::string_view process_scope = "process";
  std::atomic<std::uint64_t> next_id{1};
};

correlation_state &state() noexcept {
  static correlation_state instance;
  return instance;
}

// The calling thread's innermost live correlation_scope, or nullptr.
const correlation_scope *&innermost() noexcept {
  thread_local const correlation_scope *scope = nullptr;
  return scope;
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 when the bytes there are not one.
std::size_t utf8_sequence_length(std::string_view s, std::size_t i) noexcept {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const auto in = [](unsigned char c, unsigned lo, unsigned hi) { return c >= lo && c <= hi; };
  const unsigned char lead = byte(i);
  std::size_t len = 0;
  unsigned lo = 0x80;
  unsigned hi = 0xBF;
  if (in(lead, 0xC2, 0xDF)) {
    len = 2;
  } else if (in(lead, 0xE0, 0xEF)) {
    len = 3;
    lo = lead == 0xE0 ? 0xA0 : 0x80;
    hi = lead == 0xED ? 0x9F : 0xBF;
  } else if (in(lead, 0xF0, 0xF4)) {
    len = 4;
    lo = lead == 0xF0 ? 0x90 : 0x80;
    hi = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return 0;
  }
  if (i + len > s.size() || !in(byte(i + 1), lo, hi)) {
    return 0;
  }
  for (std::size_t k = 2; k < len; ++k) {
    if (!in(byte(i + k), 0x80, 0xBF)) {
      return 0;
    }
  }
  return len;
}

void append_uint(std::string &out, std::uint64_t v) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  out.append(buf.data(), res.ptr);
}

void append_int(std::string &out, std::int64_t v) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  out.append(buf.data(), res.ptr);
}

// Shortest round-trip form, always with a '.' or an exponent so it reads back as a number with a
// fraction; non-finite values become null (JSON has no NaN or infinity).
void append_double(std::string &out, double v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  std::array<char, 40> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  const std::string_view text{buf.data(), static_cast<std::size_t>(res.ptr - buf.data())};
  out += text;
  if (text.find_first_of(".e") == std::string_view::npos) {
    out += ".0";
  }
}

void append_iso8601(std::string &out, std::chrono::system_clock::time_point ts) {
  const auto ms = std::chrono::floor<std::chrono::milliseconds>(ts);
  std::format_to(std::back_inserter(out), "{:%Y-%m-%dT%H:%M:%S}Z", ms);
}

std::string_view basename(std::string_view path) noexcept {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view upper_level(level l) noexcept {
  switch (l) {
  case level::trace:
    return "TRACE";
  case level::debug:
    return "DEBUG";
  case level::info:
    return "INFO ";
  case level::warn:
    return "WARN ";
  case level::error:
    return "ERROR";
  case level::fatal:
    return "FATAL";
  }
  return "?????";
}

bool key_seen_before(std::span<const field> fields, std::size_t index) noexcept {
  return std::any_of(fields.begin(), fields.begin() + static_cast<std::ptrdiff_t>(index),
                     [&](const field &f) { return f.key == fields[index].key; });
}

void write_line(std::FILE *out, const std::string &line) noexcept {
  if (out == nullptr) {
    return;
  }
  std::fwrite(line.data(), 1, line.size(), out);
  std::fputc('\n', out);
}

} // namespace

// --- correlation ---

void set_process_scope(std::string_view scope) noexcept {
  correlation_state &st = state();
  const std::scoped_lock lock(st.scope_mutex);
  st.process_scope = scope;
}

std::string_view process_scope() noexcept {
  correlation_state &st = state();
  const std::scoped_lock lock(st.scope_mutex);
  return st.process_scope;
}

std::uint64_t next_correlation_id() noexcept {
  return state().next_id.fetch_add(1, std::memory_order_relaxed);
}

correlation current_correlation() noexcept {
  if (const correlation_scope *scope = innermost(); scope != nullptr) {
    return scope->get();
  }
  return correlation{.id = 0, .scope = process_scope(), .tick = 0};
}

correlation_scope::correlation_scope(correlation c) noexcept : current_(c), previous_(innermost()) {
  innermost() = this;
}

correlation_scope::correlation_scope(std::string_view scope, std::uint64_t tick) noexcept
    : correlation_scope(correlation{.id = next_correlation_id(), .scope = scope, .tick = tick}) {}

correlation_scope::~correlation_scope() { innermost() = previous_; }

// --- JSON ---

void append_json_string(std::string &out, std::string_view text) {
  static constexpr std::string_view hex = "0123456789abcdef";
  out += '"';
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80) {
      const std::size_t len = utf8_sequence_length(text, i);
      if (len == 0) {
        out += "\\ufffd";
        ++i;
      } else {
        out += text.substr(i, len);
        i += len;
      }
      continue;
    }
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        out += "\\u00";
        out += hex[c >> 4U];
        out += hex[c & 0x0FU];
      } else {
        out += static_cast<char>(c);
      }
      break;
    }
    ++i;
  }
  out += '"';
}

std::string json_sink::format(const event &e) {
  std::string out;
  out.reserve(256);
  out += R"({"schema":"sl-log-event/1","level":")";
  out += to_string(e.level);
  out += R"(","system":)";
  append_json_string(out, e.system);
  out += R"(,"message":)";
  append_json_string(out, e.message);
  out += R"(,"fields":{)";
  bool first = true;
  for (std::size_t i = 0; i < e.fields.size(); ++i) {
    if (key_seen_before(e.fields, i)) {
      continue;
    }
    if (!first) {
      out += ',';
    }
    first = false;
    append_json_string(out, e.fields[i].key);
    out += ':';
    std::visit(
        [&out](const auto &v) {
          using V = std::remove_cvref_t<decltype(v)>;
          if constexpr (std::is_same_v<V, bool>) {
            out += v ? "true" : "false";
          } else if constexpr (std::is_same_v<V, std::int64_t>) {
            append_int(out, v);
          } else if constexpr (std::is_same_v<V, double>) {
            append_double(out, v);
          } else {
            append_json_string(out, v);
          }
        },
        e.fields[i].value);
  }
  out += R"(},"correlation":{"id":)";
  append_uint(out, e.correlation.id);
  out += R"(,"scope":)";
  append_json_string(out, e.correlation.scope);
  out += R"(,"tick":)";
  append_uint(out, e.correlation.tick);
  out += R"(},"where":{"file":)";
  append_json_string(out, e.where.file_name());
  out += R"(,"line":)";
  append_uint(out, e.where.line());
  out += R"(,"function":)";
  append_json_string(out, e.where.function_name());
  out += R"(},"ts":")";
  append_iso8601(out, e.ts);
  out += R"("})";
  return out;
}

// Each write() catches at the sink boundary: a failed format or allocation drops this one event
// and counts it (sink::failures()) instead of escaping the noexcept logging path (ADR-0009).
void json_sink::write(const event &e) noexcept {
  try {
    write_line(out_, format(e));
  } catch (...) {
    record_failure();
  }
  flush();
}

void json_sink::flush() noexcept {
  if (out_ != nullptr) {
    std::fflush(out_);
  }
}

// --- text ---

std::string stderr_sink::format(const event &e) {
  std::string out;
  out.reserve(160);
  append_iso8601(out, e.ts);
  out += ' ';
  out += upper_level(e.level);
  out += " [";
  out += e.system;
  out += "] ";
  out += e.message;
  for (const field &f : e.fields) {
    out += ' ';
    out += f.key;
    out += '=';
    std::visit(
        [&out](const auto &v) {
          using V = std::remove_cvref_t<decltype(v)>;
          if constexpr (std::is_same_v<V, bool>) {
            out += v ? "true" : "false";
          } else if constexpr (std::is_same_v<V, std::int64_t>) {
            append_int(out, v);
          } else if constexpr (std::is_same_v<V, double>) {
            append_double(out, v);
          } else {
            out += '"';
            out += v;
            out += '"';
          }
        },
        f.value);
  }
  std::format_to(std::back_inserter(out), " {{scope={} id={} tick={}}} {}:{}", e.correlation.scope,
                 e.correlation.id, e.correlation.tick, basename(e.where.file_name()),
                 e.where.line());
  return out;
}

void stderr_sink::write(const event &e) noexcept {
  try {
    write_line(out_, format(e));
  } catch (...) {
    record_failure();
  }
}

void stderr_sink::flush() noexcept {
  if (out_ != nullptr) {
    std::fflush(out_);
  }
}

// --- memory ---

const memory_sink::owned_value *memory_sink::record::find(std::string_view key) const noexcept {
  for (const owned_field &f : fields) {
    if (f.key == key) {
      return &f.value;
    }
  }
  return nullptr;
}

memory_sink::memory_sink(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {
  ring_.reserve(capacity_);
}

void memory_sink::write(const event &e) noexcept {
  try {
    append(e);
  } catch (...) {
    record_failure();
  }
}

void memory_sink::append(const event &e) {
  record rec{
      .level = e.level,
      .system = std::string(e.system),
      .message = std::string(e.message),
      .fields = {},
      .correlation_id = e.correlation.id,
      .scope = std::string(e.correlation.scope),
      .tick = e.correlation.tick,
      .file = e.where.file_name(),
      .line = e.where.line(),
      .function = e.where.function_name(),
      .ts = e.ts,
  };
  rec.fields.reserve(e.fields.size());
  for (const field &f : e.fields) {
    owned_value v = std::visit(
        [](const auto &x) -> owned_value {
          using V = std::remove_cvref_t<decltype(x)>;
          if constexpr (std::is_same_v<V, std::string_view>) {
            return std::string(x);
          } else {
            return x;
          }
        },
        f.value);
    rec.fields.push_back(owned_field{.key = std::string(f.key), .value = std::move(v)});
  }
  const std::scoped_lock lock(mutex_);
  if (ring_.size() < capacity_) {
    ring_.push_back(std::move(rec));
  } else {
    ring_[next_] = std::move(rec);
  }
  next_ = (next_ + 1) % capacity_;
  ++total_;
}

std::vector<memory_sink::record> memory_sink::records() const {
  const std::scoped_lock lock(mutex_);
  if (ring_.size() < capacity_) {
    return ring_;
  }
  std::vector<record> out;
  out.reserve(ring_.size());
  for (std::size_t k = 0; k < ring_.size(); ++k) {
    out.push_back(ring_[(next_ + k) % capacity_]);
  }
  return out;
}

std::size_t memory_sink::size() const noexcept {
  const std::scoped_lock lock(mutex_);
  return ring_.size();
}

std::uint64_t memory_sink::total() const noexcept {
  const std::scoped_lock lock(mutex_);
  return total_;
}

void memory_sink::clear() noexcept {
  const std::scoped_lock lock(mutex_);
  ring_.clear();
  next_ = 0;
  total_ = 0;
}

// --- logger ---

void logger::add_sink(std::shared_ptr<sink> s) {
  if (!s) {
    return;
  }
  const std::scoped_lock lock(mutex_);
  sinks_.push_back(std::move(s));
}

bool logger::remove_sink(const sink *s) noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it =
      std::ranges::find_if(sinks_, [s](const std::shared_ptr<sink> &p) { return p.get() == s; });
  if (it == sinks_.end()) {
    return false;
  }
  sinks_.erase(it);
  return true;
}

void logger::clear_sinks() noexcept {
  const std::scoped_lock lock(mutex_);
  sinks_.clear();
}

std::size_t logger::sink_count() const noexcept {
  const std::scoped_lock lock(mutex_);
  return sinks_.size();
}

void logger::log(const event &e) noexcept {
  if (!enabled(e.level)) {
    return;
  }
  const std::scoped_lock lock(mutex_);
  for (const auto &s : sinks_) {
    s->write(e);
  }
}

void logger::flush() noexcept {
  const std::scoped_lock lock(mutex_);
  for (const auto &s : sinks_) {
    s->flush();
  }
}

namespace {
struct default_logger_holder {
  logger instance;
  default_logger_holder() { instance.add_sink(std::make_shared<stderr_sink>()); }
};
} // namespace

logger &default_logger() noexcept {
  static default_logger_holder holder;
  return holder.instance;
}

void emit(logger &target, level l, std::string_view system, std::string_view message,
          std::initializer_list<field> fields, std::source_location where) noexcept {
  if (!target.enabled(l)) {
    return;
  }
  const event e{
      .level = l,
      .system = system,
      .message = message,
      .fields = std::span<const field>{fields.begin(), fields.size()},
      .correlation = current_correlation(),
      .where = where,
      .ts = std::chrono::system_clock::now(),
  };
  target.log(e);
}

void emit(level l, std::string_view system, std::string_view message,
          std::initializer_list<field> fields, std::source_location where) noexcept {
  emit(default_logger(), l, system, message, fields, where);
}

} // namespace sl::log
