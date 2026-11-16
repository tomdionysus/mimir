//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/resp/reply_reader.h"

#include "mimir/resp/parser.h"

namespace mimir::resp {

std::optional<std::string_view> ReplyReader::line(size_t& at) {
  const size_t end = _buffer.find("\r\n", at);
  if (end == std::string::npos) {
    if (_buffer.size() - at > _limits.max_line) throw ProtocolError("reply line too long");
    return std::nullopt;
  }
  if (end - at > _limits.max_line) throw ProtocolError("reply line too long");
  std::string_view text(_buffer.data() + at, end - at);
  at = end + 2;
  return text;
}

size_t ReplyReader::count_of(std::string_view text, size_t most) {
  if (text.empty() || text.size() > 20) throw ProtocolError("invalid length");
  size_t n = 0;
  for (char c : text) {
    if (c < '0' || c > '9') throw ProtocolError("invalid length");
    n = n * 10 + static_cast<size_t>(c - '0');
    if (n > most) throw ProtocolError("length out of bounds");
  }
  return n;
}

bool ReplyReader::read(size_t& at, Item& item, size_t depth) {
  if (depth > _limits.max_depth) throw ProtocolError("reply nested too deep");
  auto head = line(at);
  if (!head) return false;
  if (head->empty()) throw ProtocolError("empty reply line");
  item.type = (*head)[0];
  const std::string_view rest = head->substr(1);
  switch (item.type) {
    case '+':
    case '-':
    case ':':
    case ',':
    case '#':
    case '(':
    case '_':
      item.text = std::string(rest);
      return true;
    case '$':
    case '!':
    case '=': {
      if (rest == "-1") return true;
      const size_t length = count_of(rest, _limits.max_bulk);
      if (_buffer.size() - at < length + 2) return false;
      if (_buffer.compare(at + length, 2, "\r\n") != 0) throw ProtocolError("bulk string without its line end");
      item.text = _buffer.substr(at, length);
      at += length + 2;
      return true;
    }
    case '*':
    case '~':
    case '>':
    case '%':
    case '|': {
      if (rest == "-1") return true;
      size_t count = count_of(rest, _limits.max_elements);
      if (item.type == '%' || item.type == '|') count *= 2;
      for (size_t i = 0; i < count; ++i) {
        Item element;
        if (!read(at, element, depth + 1)) return false;
      }
      return true;
    }
    default:
      throw ProtocolError("not a RESP reply");
  }
}

std::optional<ReplyReader::Item> ReplyReader::next() {
  size_t at = 0;
  Item item;
  if (!read(at, item, 0)) return std::nullopt;
  _buffer.erase(0, at);
  return item;
}

}  // namespace mimir::resp
