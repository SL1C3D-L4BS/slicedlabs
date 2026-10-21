// Method invocation (WO-087, SL-ARCH-003 ARCH3-R133, `error_codes` reflect_mismatch): invoke checks
// the argument count and each argument against its parameter kind before it calls the noexcept
// invoker: bool for boolean, an int64 in range for i8..i64, a uint64 in range for u8..u64, a double
// for f64 and a double that converts to float without overflow for f32, entity_bits for entity.

#include "sl/runtime/reflect.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>

namespace sl::runtime {

namespace {

bool signed_fits(std::int64_t v, field_kind k) noexcept {
  switch (k) {
  case field_kind::i8:
    return v >= std::numeric_limits<std::int8_t>::min() &&
           v <= std::numeric_limits<std::int8_t>::max();
  case field_kind::i16:
    return v >= std::numeric_limits<std::int16_t>::min() &&
           v <= std::numeric_limits<std::int16_t>::max();
  case field_kind::i32:
    return v >= std::numeric_limits<std::int32_t>::min() &&
           v <= std::numeric_limits<std::int32_t>::max();
  default:
    return true;
  }
}

bool unsigned_fits(std::uint64_t v, field_kind k) noexcept {
  switch (k) {
  case field_kind::u8:
    return v <= std::numeric_limits<std::uint8_t>::max();
  case field_kind::u16:
    return v <= std::numeric_limits<std::uint16_t>::max();
  case field_kind::u32:
    return v <= std::numeric_limits<std::uint32_t>::max();
  default:
    return true;
  }
}

bool fits_f32(double v) noexcept {
  return !std::isfinite(v) || std::fabs(v) <= double{std::numeric_limits<float>::max()};
}

bool accepts(field_kind k, const method_arg &a) noexcept {
  switch (k) {
  case field_kind::boolean:
    return std::holds_alternative<bool>(a);
  case field_kind::i8:
  case field_kind::i16:
  case field_kind::i32:
  case field_kind::i64: {
    const std::int64_t *v = std::get_if<std::int64_t>(&a);
    return v != nullptr && signed_fits(*v, k);
  }
  case field_kind::u8:
  case field_kind::u16:
  case field_kind::u32:
  case field_kind::u64: {
    const std::uint64_t *v = std::get_if<std::uint64_t>(&a);
    return v != nullptr && unsigned_fits(*v, k);
  }
  case field_kind::f32: {
    const double *v = std::get_if<double>(&a);
    return v != nullptr && fits_f32(*v);
  }
  case field_kind::f64:
    return std::holds_alternative<double>(a);
  case field_kind::entity:
    return std::holds_alternative<entity_bits>(a);
  case field_kind::record_id:
  case field_kind::bytes_fixed:
    return false;
  }
  return false;
}

} // namespace

result<method_arg> invoke(const method_info &m, void *obj,
                          std::span<const method_arg> args) noexcept {
  if (obj == nullptr || m.invoke == nullptr) {
    return make_error(errc::invalid_argument, "reflect invoke: null object or invoker");
  }
  if (args.size() != m.params.size()) {
    return make_error(errc::reflect_mismatch, "reflect invoke: wrong argument count");
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (!accepts(m.params[i], args[i])) {
      return make_error(errc::reflect_mismatch, "reflect invoke: an argument does not fit");
    }
  }
  return m.invoke(obj, args);
}

} // namespace sl::runtime
