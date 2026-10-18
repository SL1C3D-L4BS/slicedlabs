#pragma once

// Golden-file helper for doctest (WO-027, SL-EP-052). Header-only.
//
//   SL_GOLDEN_CHECK("name", text);   // compares `text` with tests/golden/name.golden
//
// The golden directory comes from the compile definition SL_GOLDEN_DIR (set by
// tests/CMakeLists.txt). On a mismatch the check fails and prints a unified-style line diff (`---
// golden`, `+++ actual`, `@@` hunks). With the environment variable SL_UPDATE_GOLDEN=1 the golden
// file is rewritten from `text` and the check passes. Include after, or instead of,
// <doctest/doctest.h>.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SL_GOLDEN_DIR
#error "SL_GOLDEN_DIR must be defined (tests/CMakeLists.txt sets it for golden tests)"
#endif

namespace sl::golden {

inline std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

/// Unified-style diff of two texts (empty when they are equal). Lines come from a
/// longest-common-subsequence walk; every changed line is shown with up to `context` unchanged
/// lines around it.
inline std::string line_diff(std::string_view expected, std::string_view actual,
                             std::string_view label, std::size_t context = 2) {
  if (expected == actual) {
    return {};
  }
  const std::vector<std::string> a = split_lines(expected);
  const std::vector<std::string> b = split_lines(actual);
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  std::vector<std::vector<std::size_t>> lcs(n + 1, std::vector<std::size_t>(m + 1, 0));
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    }
  }
  struct Row {
    char tag;
    const std::string *text;
  };
  std::vector<Row> rows;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && a[i] == b[j]) {
      rows.push_back({' ', &a[i]});
      ++i;
      ++j;
    } else if (i < n && (j == m || lcs[i + 1][j] >= lcs[i][j + 1])) {
      rows.push_back({'-', &a[i]});
      ++i;
    } else {
      rows.push_back({'+', &b[j]});
      ++j;
    }
  }
  std::vector<bool> show(rows.size(), false);
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].tag != ' ') {
      const std::size_t from = r > context ? r - context : 0;
      const std::size_t to = r + context < rows.size() ? r + context : rows.size() - 1;
      for (std::size_t k = from; k <= to; ++k) {
        show[k] = true;
      }
    }
  }
  std::ostringstream out;
  out << "--- golden/" << label << ".golden\n+++ actual\n";
  std::size_t old_line = 1;
  std::size_t new_line = 1;
  bool in_hunk = false;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (show[r]) {
      if (!in_hunk) {
        out << "@@ -" << old_line << " +" << new_line << " @@\n";
        in_hunk = true;
      }
      out << rows[r].tag << *rows[r].text << '\n';
    } else {
      in_hunk = false;
    }
    if (rows[r].tag != '+') {
      ++old_line;
    }
    if (rows[r].tag != '-') {
      ++new_line;
    }
  }
  if (!expected.empty() && expected.back() != '\n') {
    out << "\\ golden file has no newline at end of file\n";
  }
  if (!actual.empty() && actual.back() != '\n') {
    out << "\\ actual text has no newline at end of file\n";
  }
  return out.str();
}

inline bool update_requested() {
  const char *value = std::getenv("SL_UPDATE_GOLDEN");
  return value != nullptr && std::string_view{value} == "1";
}

/// Compares `text` with <SL_GOLDEN_DIR>/<name>.golden. Returns true when they match (or the golden
/// was rewritten); otherwise false with the explanation, including the diff, in `message`.
inline bool compare(std::string_view name, std::string_view text, std::string &message) {
  const std::string path = std::string{SL_GOLDEN_DIR} + "/" + std::string{name} + ".golden";
  if (update_requested()) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
      message = "golden '" + std::string{name} + "': cannot write " + path;
      return false;
    }
    return true;
  }
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    message = "golden '" + std::string{name} + "': " + path +
              " does not exist; create it with SL_UPDATE_GOLDEN=1";
    return false;
  }
  const std::string expected{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  if (expected == text) {
    return true;
  }
  message = "golden '" + std::string{name} + "' differs from " + path +
            " (SL_UPDATE_GOLDEN=1 rewrites it)\n" + line_diff(expected, text, name);
  return false;
}

} // namespace sl::golden

#define SL_GOLDEN_CHECK(name, text)                                                                \
  do {                                                                                             \
    std::string sl_golden_message;                                                                 \
    const bool sl_golden_ok = ::sl::golden::compare((name), (text), sl_golden_message);            \
    CHECK_MESSAGE(sl_golden_ok, "\n" << sl_golden_message);                                        \
  } while (false)
