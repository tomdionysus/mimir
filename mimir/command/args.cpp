//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/args.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mimir::command {

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::optional<int64_t> parse_integer(std::string_view text) noexcept {
  if (text.empty() || text.size() > 20) return std::nullopt;
  size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
    if (text.size() == 1) return std::nullopt;
  }
  if (text[i] == '0') {
    // "0" alone; "-0" and leading zeros are not integers to Valkey.
    if (text.size() == 1) return 0;
    return std::nullopt;
  }
  uint64_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return std::nullopt;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  if (negative) {
    if (value > static_cast<uint64_t>(INT64_MAX) + 1) return std::nullopt;
    return static_cast<int64_t>(0 - value);
  }
  if (value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  return static_cast<int64_t>(value);
}

int64_t integer_argument(std::string_view text) {
  auto value = parse_integer(text);
  if (!value) throw CommandError(not_integer);
  return *value;
}

std::optional<long double> parse_long_double(std::string_view text) noexcept {
  // Valkey's bound on a number's text, and no leading space (strtold would skip it).
  if (text.empty() || text.size() > 5120 || std::isspace(static_cast<unsigned char>(text[0]))) return std::nullopt;
  const std::string copy(text);
  char* end = nullptr;
  errno = 0;
  const long double value = std::strtold(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || errno == ERANGE || std::isnan(value)) return std::nullopt;
  return value;
}

std::string format_long_double(long double value) {
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  char text[5600];
  int length = std::snprintf(text, sizeof(text), "%.17Lf", value);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) return "0";
  std::string out(text, static_cast<size_t>(length));
  if (out.find('.') != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  if (out == "-0") out = "0";
  return out;
}

std::string format_integer(int64_t value) { return std::to_string(value); }

}  // namespace mimir::command
