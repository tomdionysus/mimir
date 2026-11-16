//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Replies from another RESP server, read whole one at a time, for MIGRATE's
// talk with its target. Bytes from outside the process: every length is
// bounded before anything is kept, nesting is bounded, and anything that is
// not RESP2 or RESP3 throws resp::ProtocolError.
namespace mimir::resp {

struct ReplyLimits {
  size_t max_line = 64 * 1024;
  size_t max_bulk = 1u << 20;
  size_t max_elements = 1u << 16;
  size_t max_depth = 8;
};

class ReplyReader {
 public:
  using Limits = ReplyLimits;

  // One reply: its type byte ('+', '-', ':', '$', '*', ...) and, for a
  // simple string, an error, an integer or a bulk string, its text. An
  // aggregate's elements are read and passed over.
  struct Item {
    char type = 0;
    std::string text;
  };

  explicit ReplyReader(Limits limits = {}) : _limits(limits) {}

  void feed(std::string_view bytes) { _buffer.append(bytes); }
  // The next whole reply, or none until more arrives.
  std::optional<Item> next();
  size_t buffered() const noexcept { return _buffer.size(); }

 private:
  // Reads one reply at `at`; false when it is not all here yet.
  bool read(size_t& at, Item& item, size_t depth);
  // The line at `at`, without its CRLF; none when it is not all here yet.
  std::optional<std::string_view> line(size_t& at);
  static size_t count_of(std::string_view text, size_t most);

  Limits _limits;
  std::string _buffer;
};

}  // namespace mimir::resp
