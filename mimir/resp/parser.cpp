//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/resp/parser.h"

#include <charconv>
#include <cstdint>

namespace mimir::resp {

namespace {

// A header line's number: an optional '-' and digits, nothing else.
std::optional<int64_t> header_number(std::string_view text) {
  if (text.empty()) return std::nullopt;
  int64_t value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return value;
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

bool is_space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f'; }

}  // namespace

std::vector<std::string> split_inline(std::string_view line) {
  std::vector<std::string> words;
  size_t i = 0;
  const auto unbalanced = [] { throw ProtocolError("Protocol error: unbalanced quotes in request"); };
  for (;;) {
    while (i < line.size() && is_space(line[i])) ++i;
    if (i == line.size()) return words;
    std::string word;
    bool in_double = false;
    bool in_single = false;
    for (;;) {
      if (in_double) {
        if (i == line.size()) unbalanced();
        char c = line[i];
        if (c == '\\' && i + 3 < line.size() && line[i + 1] == 'x' && is_hex(line[i + 2]) && is_hex(line[i + 3])) {
          word.push_back(static_cast<char>(hex_value(line[i + 2]) * 16 + hex_value(line[i + 3])));
          i += 4;
        } else if (c == '\\' && i + 1 < line.size()) {
          char e = line[i + 1];
          switch (e) {
            case 'n':
              word.push_back('\n');
              break;
            case 'r':
              word.push_back('\r');
              break;
            case 't':
              word.push_back('\t');
              break;
            case 'b':
              word.push_back('\b');
              break;
            case 'a':
              word.push_back('\a');
              break;
            default:
              word.push_back(e);
              break;
          }
          i += 2;
        } else if (c == '"') {
          ++i;
          // A closing quote must end the word.
          if (i < line.size() && !is_space(line[i])) unbalanced();
          break;
        } else {
          word.push_back(c);
          ++i;
        }
      } else if (in_single) {
        if (i == line.size()) unbalanced();
        char c = line[i];
        if (c == '\\' && i + 1 < line.size() && line[i + 1] == '\'') {
          word.push_back('\'');
          i += 2;
        } else if (c == '\'') {
          ++i;
          if (i < line.size() && !is_space(line[i])) unbalanced();
          break;
        } else {
          word.push_back(c);
          ++i;
        }
      } else {
        if (i == line.size() || is_space(line[i])) break;
        char c = line[i];
        if (c == '"') {
          in_double = true;
        } else if (c == '\'') {
          in_single = true;
        } else {
          word.push_back(c);
        }
        ++i;
      }
    }
    words.push_back(std::move(word));
  }
}

RequestParser::RequestParser(ParserLimits limits) : _limits(limits) {}

void RequestParser::feed(std::string_view bytes) { _buffer.append(bytes); }

void RequestParser::fail(const std::string& message) {
  _failed = message;
  throw ProtocolError(message);
}

void RequestParser::compact() {
  // Drop what has been consumed once it is most of the buffer, so a long
  // pipeline does not hold every request it has already served.
  if (_position > 0 && (_position == _buffer.size() || _position >= 4096)) {
    _buffer.erase(0, _position);
    _position = 0;
  }
}

std::optional<std::vector<std::string>> RequestParser::next() {
  if (_failed) throw ProtocolError(*_failed);
  for (;;) {
    std::optional<std::vector<std::string>> command;
    if (_count || (_position < _buffer.size() && _buffer[_position] == '*')) {
      command = next_array();
    } else {
      command = next_inline();
    }
    compact();
    if (!command) return std::nullopt;
    // An empty array or an empty line is no command.
    if (!command->empty()) return command;
    if (_position >= _buffer.size()) return std::nullopt;
  }
}

std::optional<std::vector<std::string>> RequestParser::next_inline() {
  if (_position >= _buffer.size()) return std::nullopt;
  const size_t newline = _buffer.find('\n', _position);
  if (newline == std::string::npos) {
    if (_buffer.size() - _position > _limits.max_inline) fail("Protocol error: too big inline request");
    return std::nullopt;
  }
  size_t end = newline;
  if (end > _position && _buffer[end - 1] == '\r') --end;
  if (end - _position > _limits.max_inline) fail("Protocol error: too big inline request");
  std::string_view line(_buffer.data() + _position, end - _position);
  std::vector<std::string> words;
  try {
    words = split_inline(line);
  } catch (const ProtocolError& e) {
    fail(e.what());
  }
  // The same count an array is held to.
  if (words.size() > (_restricted ? _limits.restricted_arguments : _limits.max_arguments)) {
    fail(_restricted ? "Protocol error: unauthenticated multibulk length" : "Protocol error: invalid multibulk length");
  }
  _position = newline + 1;
  return words;
}

std::optional<std::vector<std::string>> RequestParser::next_array() {
  const size_t max_arguments = _restricted ? _limits.restricted_arguments : _limits.max_arguments;
  const size_t max_bulk = _restricted ? _limits.restricted_bulk : _limits.max_bulk;

  // A header line: its text between the type byte and "\r\n", or none until
  // it has arrived; too long a wait for its end is refused.
  const auto header = [&](const char* too_big) -> std::optional<std::string_view> {
    const size_t cr = _buffer.find("\r\n", _position);
    if (cr == std::string::npos) {
      if (_buffer.size() - _position > _limits.max_inline) fail(too_big);
      return std::nullopt;
    }
    if (cr - _position > _limits.max_inline) fail(too_big);
    std::string_view text(_buffer.data() + _position + 1, cr - _position - 1);
    _position = cr + 2;
    return text;
  };

  if (!_count) {
    auto text = header("Protocol error: too big mbulk count string");
    if (!text) return std::nullopt;
    auto count = header_number(*text);
    if (!count) fail("Protocol error: invalid multibulk length");
    if (*count <= 0) return std::vector<std::string>{};
    if (static_cast<uint64_t>(*count) > max_arguments) {
      fail(_restricted ? "Protocol error: unauthenticated multibulk length" : "Protocol error: invalid multibulk length");
    }
    _count = static_cast<size_t>(*count);
    _arguments.clear();
    // Reserve no more than a modest number up front: the count is the client's
    // word, not yet backed by bytes.
    _arguments.reserve(std::min<size_t>(*_count, 1024));
  }

  while (_arguments.size() < *_count) {
    if (!_bulk) {
      if (_position >= _buffer.size()) return std::nullopt;
      if (_buffer[_position] != '$') {
        fail(std::string("Protocol error: expected '$', got '") + _buffer[_position] + "'");
      }
      auto text = header("Protocol error: too big bulk count string");
      if (!text) return std::nullopt;
      auto length = header_number(*text);
      if (!length || *length < 0) fail("Protocol error: invalid bulk length");
      if (static_cast<uint64_t>(*length) > max_bulk) {
        fail(_restricted ? "Protocol error: unauthenticated bulk length" : "Protocol error: invalid bulk length");
      }
      _bulk = static_cast<size_t>(*length);
    }
    // The line end after the bytes is checked as soon as each byte of it is
    // here; until the whole is here it is waited for, and the buffer grows only
    // with what the client actually sends.
    const size_t available = _buffer.size() - _position;
    if ((available > *_bulk && _buffer[_position + *_bulk] != '\r') || (available > *_bulk + 1 && _buffer[_position + *_bulk + 1] != '\n')) {
      fail("Protocol error: expected '\\r\\n' after bulk string");
    }
    if (available < *_bulk + 2) return std::nullopt;
    _arguments.emplace_back(_buffer, _position, *_bulk);
    _position += *_bulk + 2;
    _bulk.reset();
  }
  _count.reset();
  return std::move(_arguments);
}

}  // namespace mimir::resp
