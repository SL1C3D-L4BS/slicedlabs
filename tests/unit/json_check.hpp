#pragma once

// A tiny strict JSON syntax checker for the logging tests (RFC 8259 grammar, no extensions). It
// only answers "is this one well-formed JSON value"; tests compare content with expected strings.

#include <cstddef>
#include <string_view>

namespace sl::test {

class json_checker {
public:
  explicit json_checker(std::string_view text) : s_(text) {}

  /// True when the whole text is exactly one JSON value (surrounding whitespace allowed).
  [[nodiscard]] bool valid() {
    ws();
    if (!value(0)) {
      return false;
    }
    ws();
    return i_ == s_.size();
  }

private:
  static constexpr int kMaxDepth = 64;

  [[nodiscard]] bool at_end() const { return i_ >= s_.size(); }
  [[nodiscard]] char peek() const { return at_end() ? '\0' : s_[i_]; }
  void ws() {
    while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
      ++i_;
    }
  }
  bool eat(char c) {
    if (peek() != c) {
      return false;
    }
    ++i_;
    return true;
  }
  bool literal(std::string_view word) {
    if (s_.substr(i_, word.size()) != word) {
      return false;
    }
    i_ += word.size();
    return true;
  }
  static bool is_digit(char c) { return c >= '0' && c <= '9'; }
  static bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }

  bool value(int depth) {
    if (depth > kMaxDepth) {
      return false;
    }
    switch (peek()) {
    case '{':
      return object(depth);
    case '[':
      return array(depth);
    case '"':
      return string();
    case 't':
      return literal("true");
    case 'f':
      return literal("false");
    case 'n':
      return literal("null");
    default:
      return number();
    }
  }

  bool object(int depth) {
    ++i_;
    ws();
    if (eat('}')) {
      return true;
    }
    for (;;) {
      ws();
      if (!string()) {
        return false;
      }
      ws();
      if (!eat(':')) {
        return false;
      }
      ws();
      if (!value(depth + 1)) {
        return false;
      }
      ws();
      if (eat('}')) {
        return true;
      }
      if (!eat(',')) {
        return false;
      }
    }
  }

  bool array(int depth) {
    ++i_;
    ws();
    if (eat(']')) {
      return true;
    }
    for (;;) {
      ws();
      if (!value(depth + 1)) {
        return false;
      }
      ws();
      if (eat(']')) {
        return true;
      }
      if (!eat(',')) {
        return false;
      }
    }
  }

  bool string() {
    if (!eat('"')) {
      return false;
    }
    while (!at_end()) {
      const auto c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') {
        return true;
      }
      if (c < 0x20) {
        return false;
      }
      if (c == '\\') {
        const char e = peek();
        ++i_;
        if (e == 'u') {
          for (int k = 0; k < 4; ++k) {
            if (!is_hex(peek())) {
              return false;
            }
            ++i_;
          }
        } else if (std::string_view{"\"\\/bfnrt"}.find(e) == std::string_view::npos) {
          return false;
        }
      }
    }
    return false;
  }

  bool digits() {
    const std::size_t start = i_;
    while (is_digit(peek())) {
      ++i_;
    }
    return i_ > start;
  }

  bool number() {
    eat('-');
    if (eat('0')) {
      if (is_digit(peek())) {
        return false;
      }
    } else if (!digits()) {
      return false;
    }
    if (eat('.') && !digits()) {
      return false;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++i_;
      if (!eat('+')) {
        eat('-');
      }
      if (!digits()) {
        return false;
      }
    }
    return true;
  }

  std::string_view s_;
  std::size_t i_ = 0;
};

[[nodiscard]] inline bool json_valid(std::string_view text) { return json_checker{text}.valid(); }

} // namespace sl::test
