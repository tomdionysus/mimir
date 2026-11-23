//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/resp/reply.h"

#include <array>
#include <charconv>
#include <cmath>

namespace mimir::resp {

namespace {

void append_line(std::string& out, std::string_view text) {
  for (char c : text) out.push_back(c == '\r' || c == '\n' ? ' ' : c);
  out.append("\r\n");
}

}  // namespace

std::string format_double(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  std::array<char, 64> text{};
  auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
  (void)error;
  return std::string(text.data(), end);
}

void Reply::header(char type, int64_t count) {
  _bytes.push_back(type);
  _bytes.append(std::to_string(count));
  _bytes.append("\r\n");
}

void Reply::simple(std::string_view text) {
  _bytes.push_back('+');
  append_line(_bytes, text);
}

void Reply::error(std::string_view text) {
  _bytes.push_back('-');
  append_line(_bytes, text);
}

void Reply::integer(int64_t value) { header(':', value); }

void Reply::bulk(std::string_view bytes) {
  header('$', static_cast<int64_t>(bytes.size()));
  _bytes.append(bytes);
  _bytes.append("\r\n");
}

void Reply::null() {
  if (_protocol == Protocol::resp3)
    _bytes.append("_\r\n");
  else
    _bytes.append("$-1\r\n");
}

void Reply::null_array() {
  if (_protocol == Protocol::resp3)
    _bytes.append("_\r\n");
  else
    _bytes.append("*-1\r\n");
}

void Reply::array(size_t count) { header('*', static_cast<int64_t>(count)); }

void Reply::map(size_t pairs) {
  if (_protocol == Protocol::resp3)
    header('%', static_cast<int64_t>(pairs));
  else
    header('*', static_cast<int64_t>(pairs * 2));
}

void Reply::set(size_t count) { header(_protocol == Protocol::resp3 ? '~' : '*', static_cast<int64_t>(count)); }

void Reply::push(size_t count) { header(_protocol == Protocol::resp3 ? '>' : '*', static_cast<int64_t>(count)); }

void Reply::real(double value) {
  if (_protocol == Protocol::resp3) {
    _bytes.push_back(',');
    _bytes.append(format_double(value));
    _bytes.append("\r\n");
  } else {
    bulk(format_double(value));
  }
}

void Reply::boolean(bool value) {
  if (_protocol == Protocol::resp3)
    _bytes.append(value ? "#t\r\n" : "#f\r\n");
  else
    integer(value ? 1 : 0);
}

void Reply::big_number(std::string_view digits) {
  if (_protocol == Protocol::resp3) {
    // A script gives the digits: a line end inside them would end the reply
    // early, so it becomes a space, as in an error.
    _bytes.push_back('(');
    for (const char c : digits) _bytes.push_back(c == '\r' || c == '\n' ? ' ' : c);
    _bytes.append("\r\n");
  } else {
    bulk(digits);
  }
}

void Reply::verbatim(std::string_view format, std::string_view text) {
  if (_protocol == Protocol::resp3) {
    header('=', static_cast<int64_t>(format.size() + 1 + text.size()));
    _bytes.append(format);
    _bytes.push_back(':');
    _bytes.append(text);
    _bytes.append("\r\n");
  } else {
    bulk(text);
  }
}

}  // namespace mimir::resp
