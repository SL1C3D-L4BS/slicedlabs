// Checkpoint files of the commit log (WO-055, SL-ARCH-002 `journal` checkpoint, ARCH2-R32).
// See checkpoint.hpp for the format.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t head_size = 6; // magic, major, minor
constexpr std::size_t sum_size = sha256::digest_size;
constexpr std::string_view name_prefix = "ckpt-";
constexpr std::string_view name_suffix = ".slc";
constexpr std::size_t name_hex = 16;

std::unexpected<error> invalid(std::string_view detail) {
  std::string m("checkpoint: ");
  m += detail;
  return make_error(errc::checkpoint_invalid, std::move(m));
}

std::unexpected<error> bad_input(std::string_view detail) {
  std::string m("encode_checkpoint: ");
  m += detail;
  return make_error(errc::invalid_argument, std::move(m));
}

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

// A field of the checkpoint struct of the given kind, or nullptr when absent and optional.
result<const value *> field_of(const value &s, std::uint32_t no, value_kind kind, bool required) {
  const value *f = s.find_field(no);
  if (f == nullptr) {
    if (required) {
      return invalid("field " + std::to_string(no) + " is missing");
    }
    return static_cast<const value *>(nullptr);
  }
  if (f->kind() != kind) {
    return invalid("field " + std::to_string(no) + " is " + std::string(to_string(f->kind())));
  }
  return f;
}

result<value> ref_value(const ref_entry &r) {
  result<value> name = value::string(r.name);
  if (!name) {
    return pass(name);
  }
  std::vector<field> f;
  f.push_back(field{.no = 1, .v = std::move(*name)});
  f.push_back(field{.no = 2, .v = value::digest(r.target)});
  return value::struct_(std::move(f));
}

result<value> intent_value(const checkpoint_intent &in) {
  result<value> kind = value::string(in.kind);
  if (!kind) {
    return pass(kind);
  }
  std::vector<field> f;
  f.push_back(field{.no = 1, .v = value::id(in.operation)});
  f.push_back(field{.no = 2, .v = std::move(*kind)});
  return value::struct_(std::move(f));
}

result<ref_entry> decode_ref(const value &item) {
  if (item.kind() != value_kind::struct_) {
    return invalid("a ref is not a struct");
  }
  const result<const value *> name = field_of(item, 1, value_kind::string, true);
  if (!name) {
    return pass(name);
  }
  const result<const value *> target = field_of(item, 2, value_kind::digest, true);
  if (!target) {
    return pass(target);
  }
  return ref_entry{.name = std::string((*name)->as_string()), .target = (*target)->as_digest()};
}

result<checkpoint_intent> decode_intent_entry(const value &item) {
  if (item.kind() != value_kind::struct_) {
    return invalid("an open intent is not a struct");
  }
  const result<const value *> op = field_of(item, 1, value_kind::id, true);
  if (!op) {
    return pass(op);
  }
  const result<const value *> kind = field_of(item, 2, value_kind::string, true);
  if (!kind) {
    return pass(kind);
  }
  const result<record_id> id = (*op)->as_record_id();
  if (!id || id->is_null()) {
    return invalid("an open intent has no operation id");
  }
  return checkpoint_intent{.operation = *id, .kind = std::string((*kind)->as_string())};
}

bool sorted_refs(const std::vector<ref_entry> &refs) {
  return std::ranges::adjacent_find(refs, [](const ref_entry &a, const ref_entry &b) {
           return a.name >= b.name;
         }) == refs.end();
}

bool sorted_intents(const std::vector<checkpoint_intent> &in) {
  return std::ranges::adjacent_find(in, [](const checkpoint_intent &a, const checkpoint_intent &b) {
           return !(a.operation < b.operation);
         }) == in.end();
}

result<void> sync_file(const vfs_file &f, durability d) noexcept {
  switch (d) {
  case durability::none:
    return {};
  case durability::flush:
    return f.fdatasync();
  case durability::fsync:
    return f.fsync();
  }
  return {};
}

} // namespace

std::string checkpoint_file_name(std::uint64_t seq) {
  std::array<char, name_hex> hex{};
  hex.fill('0');
  std::array<char, name_hex> digits{};
  const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), seq, 16);
  const auto n = static_cast<std::size_t>(end - digits.data());
  if (ec == std::errc{}) {
    std::copy_n(digits.begin(), n, hex.begin() + static_cast<std::ptrdiff_t>(name_hex - n));
  }
  std::string out(name_prefix);
  out.append(hex.data(), hex.size());
  out += name_suffix;
  return out;
}

std::optional<std::uint64_t> parse_checkpoint_file_name(std::string_view name) noexcept {
  if (name.size() != name_prefix.size() + name_hex + name_suffix.size() ||
      !name.starts_with(name_prefix) || !name.ends_with(name_suffix)) {
    return std::nullopt;
  }
  const std::string_view hex = name.substr(name_prefix.size(), name_hex);
  for (const char c : hex) {
    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) {
      return std::nullopt;
    }
  }
  std::uint64_t seq = 0;
  const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), seq, 16);
  if (ec != std::errc{} || end != hex.data() + hex.size()) {
    return std::nullopt;
  }
  return seq;
}

result<std::vector<std::uint8_t>> encode_checkpoint(const checkpoint &c) {
  if (!sorted_refs(c.refs)) {
    return bad_input("refs are not sorted and unique by name");
  }
  if (!sorted_intents(c.open_intents)) {
    return bad_input("open intents are not sorted and unique by operation");
  }
  if (c.frame_offset >= c.offset) {
    return bad_input("the commit frame offset is not below the offset after it");
  }
  std::vector<field> f;
  f.push_back(field{.no = 1, .v = value::uint(c.seq)});
  f.push_back(field{.no = 2, .v = value::uint(c.segment)});
  f.push_back(field{.no = 3, .v = value::uint(c.offset)});
  f.push_back(field{.no = 4, .v = value::digest(c.last_commit)});
  std::vector<value> refs;
  refs.reserve(c.refs.size());
  for (const ref_entry &r : c.refs) {
    result<value> rv = ref_value(r);
    if (!rv) {
      return pass(rv);
    }
    refs.push_back(std::move(*rv));
  }
  result<value> ref_list = value::list(std::move(refs));
  if (!ref_list) {
    return pass(ref_list);
  }
  f.push_back(field{.no = 5, .v = std::move(*ref_list)});
  if (c.schema_set) {
    f.push_back(field{.no = 6, .v = value::digest(*c.schema_set)});
  }
  std::vector<value> intents;
  intents.reserve(c.open_intents.size());
  for (const checkpoint_intent &in : c.open_intents) {
    result<value> iv = intent_value(in);
    if (!iv) {
      return pass(iv);
    }
    intents.push_back(std::move(*iv));
  }
  result<value> intent_list = value::list(std::move(intents));
  if (!intent_list) {
    return pass(intent_list);
  }
  f.push_back(field{.no = 7, .v = std::move(*intent_list)});
  f.push_back(field{.no = 8, .v = value::uint(c.frame_offset)});
  result<value> s = value::struct_(std::move(f));
  if (!s) {
    return pass(s);
  }
  result<std::vector<std::uint8_t>> body = encode(*s);
  if (!body) {
    return pass(body);
  }
  std::vector<std::uint8_t> out(checkpoint_magic.begin(), checkpoint_magic.end());
  out.push_back(checkpoint_major);
  out.push_back(checkpoint_minor);
  out.insert(out.end(), body->begin(), body->end());
  const sha256::value_type sum = sha256::hash(out);
  out.insert(out.end(), sum.begin(), sum.end());
  return out;
}

result<checkpoint> decode_checkpoint(byte_span bytes) {
  if (bytes.size() < head_size + sum_size) {
    return invalid(std::to_string(bytes.size()) + " bytes is too short");
  }
  if (!std::ranges::equal(bytes.first(checkpoint_magic.size()), checkpoint_magic)) {
    return invalid("bad magic");
  }
  const byte_span covered = bytes.first(bytes.size() - sum_size);
  const sha256::value_type sum = sha256::hash(covered);
  if (!std::ranges::equal(bytes.last(sum_size), sum)) {
    return invalid("checksum mismatch");
  }
  if (bytes[4] != checkpoint_major) {
    return invalid("major " + std::to_string(bytes[4]) + ", supported " +
                   std::to_string(checkpoint_major));
  }
  const result<value> s = decode(covered.subspan(head_size));
  if (!s) {
    return invalid("payload does not decode: " + s.error().message);
  }
  if (s->kind() != value_kind::struct_) {
    return invalid("payload is not a struct");
  }
  checkpoint c;
  const result<const value *> seq = field_of(*s, 1, value_kind::uint, true);
  const result<const value *> segment = field_of(*s, 2, value_kind::uint, true);
  const result<const value *> offset = field_of(*s, 3, value_kind::uint, true);
  const result<const value *> last = field_of(*s, 4, value_kind::digest, true);
  const result<const value *> refs = field_of(*s, 5, value_kind::list, true);
  const result<const value *> schema = field_of(*s, 6, value_kind::digest, false);
  const result<const value *> intents = field_of(*s, 7, value_kind::list, true);
  const result<const value *> frame = field_of(*s, 8, value_kind::uint, true);
  for (const result<const value *> *r :
       {&seq, &segment, &offset, &last, &refs, &schema, &intents, &frame}) {
    if (!*r) {
      return pass(*r);
    }
  }
  c.seq = (*seq)->as_uint();
  if ((*segment)->as_uint() == 0 || (*segment)->as_uint() > 0xFFFF'FFFFU) {
    return invalid("segment " + std::to_string((*segment)->as_uint()) + " is out of range");
  }
  c.segment = static_cast<std::uint32_t>((*segment)->as_uint());
  c.offset = (*offset)->as_uint();
  c.frame_offset = (*frame)->as_uint();
  c.last_commit = (*last)->as_digest();
  if (*schema != nullptr) {
    c.schema_set = (*schema)->as_digest();
  }
  for (const value &item : (*refs)->items()) {
    result<ref_entry> r = decode_ref(item);
    if (!r) {
      return pass(r);
    }
    c.refs.push_back(std::move(*r));
  }
  for (const value &item : (*intents)->items()) {
    result<checkpoint_intent> in = decode_intent_entry(item);
    if (!in) {
      return pass(in);
    }
    c.open_intents.push_back(std::move(*in));
  }
  if (c.seq == 0) {
    return invalid("seq 0 names no commit");
  }
  if (!sorted_refs(c.refs) || !sorted_intents(c.open_intents)) {
    return invalid("refs or open intents are not sorted and unique");
  }
  if (c.frame_offset >= c.offset) {
    return invalid("the commit frame offset is not below the offset after it");
  }
  return c;
}

result<void> write_checkpoint(vfs &fs, const std::filesystem::path &store_dir, const checkpoint &c,
                              durability sync) noexcept {
  const result<std::vector<std::uint8_t>> bytes = encode_checkpoint(c);
  if (!bytes) {
    return pass(bytes);
  }
  const std::filesystem::path dir = store_dir / checkpoint_dir_name;
  const result<bool> present = fs.exists(dir);
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    if (result<void> ok = fs.make_dir(dir); !ok && ok.error().code != errc::already_exists) {
      return ok;
    }
    if (sync != durability::none) {
      if (result<void> ok = fs.sync_dir(store_dir); !ok) {
        return ok;
      }
    }
  }
  const std::filesystem::path tmp = dir / "ckpt.tmp";
  {
    result<vfs_file> f = fs.open(tmp, open_create);
    if (!f) {
      return pass(f);
    }
    if (result<std::uint64_t> at = f->append(*bytes); !at) {
      return pass(at);
    }
    if (result<void> ok = sync_file(*f, sync); !ok) {
      return ok;
    }
  }
  if (result<void> ok = fs.rename(tmp, dir / checkpoint_file_name(c.seq)); !ok) {
    return ok;
  }
  if (sync == durability::none) {
    return {};
  }
  return fs.sync_dir(dir);
}

result<std::vector<std::string>> list_checkpoints(vfs &fs,
                                                  const std::filesystem::path &store_dir) noexcept {
  const std::filesystem::path dir = store_dir / checkpoint_dir_name;
  const result<bool> present = fs.exists(dir);
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return std::vector<std::string>{};
  }
  result<std::vector<std::string>> names = fs.list(dir);
  if (!names) {
    return names;
  }
  std::vector<std::pair<std::uint64_t, std::string>> found;
  for (std::string &name : *names) {
    if (const std::optional<std::uint64_t> seq = parse_checkpoint_file_name(name)) {
      found.emplace_back(*seq, std::move(name));
    }
  }
  std::ranges::sort(found, [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<std::string> out;
  out.reserve(found.size());
  for (auto &entry : found) {
    out.push_back(std::move(entry.second));
  }
  return out;
}

} // namespace sl::fabric
