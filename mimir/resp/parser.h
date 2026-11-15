//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The request side of RESP: what a client sends, which is either an array of
// bulk strings ("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n") or an inline command, a line
// of space-separated words with Valkey's quoting ("GET k\r\n"). Bytes arrive
// from outside the process, so this is a parser of untrusted input: every
// length is bounded before anything is allocated for it, and what is not a
// request is refused with the words Valkey's clients expect, after which the
// connection is closed.
namespace mimir::resp {

// Bytes that are not a request. what() is the reply's text after "ERR ", as
// Valkey words it ("Protocol error: invalid bulk length").
class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ParserLimits {
  // Arguments in one command (Valkey takes up to INT_MAX; Mimir bounds it).
  size_t max_arguments = 1024 * 1024;
  // One argument's bytes: Valkey's proto-max-bulk-len.
  size_t max_bulk = 512u * 1024 * 1024;
  // An inline command's line, or an array's or bulk string's header line,
  // without its line end: Valkey's PROTO_INLINE_MAX_SIZE.
  size_t max_inline = 64 * 1024;
  // For a connection not yet authenticated where authentication is required,
  // as Valkey bounds it: 10 arguments of 16 KiB.
  size_t restricted_arguments = 10;
  size_t restricted_bulk = 16 * 1024;
};

// One connection's requests, taken from its bytes in order. Not thread-safe:
// one connection's strand owns it.
class RequestParser {
 public:
  explicit RequestParser(ParserLimits limits = {});

  // Appends bytes received.
  void feed(std::string_view bytes);
  // The next whole command's arguments, the command's name first; none until
  // one has arrived whole. An empty inline line and an empty array are no
  // command and are skipped, as Valkey skips them. Throws ProtocolError for
  // bytes that are not a request; the parser is then spent and throws the
  // same error again.
  std::optional<std::vector<std::string>> next();

  // While true, the restricted limits apply (an unauthenticated connection).
  void restrict(bool on) noexcept { _restricted = on; }
  // Bytes held: received and not yet part of a command returned.
  size_t buffered() const noexcept { return _buffer.size() - _position; }

 private:
  std::optional<std::vector<std::string>> next_inline();
  std::optional<std::vector<std::string>> next_array();
  [[noreturn]] void fail(const std::string& message);
  void compact();

  ParserLimits _limits;
  bool _restricted = false;
  std::string _buffer;
  size_t _position = 0;
  std::optional<std::string> _failed;

  // An array being read: its count, and the arguments read so far.
  std::optional<size_t> _count;
  std::vector<std::string> _arguments;
  // The current bulk string's length once its header is read.
  std::optional<size_t> _bulk;
};

// Splits an inline command's line into words as Valkey does: spaces separate;
// "..." takes \n \r \t \b \a \\ \" and \xHH escapes; '...' takes \' only; a
// closing quote must be followed by a space or the end. Throws ProtocolError
// ("Protocol error: unbalanced quotes in request") otherwise.
std::vector<std::string> split_inline(std::string_view line);

}  // namespace mimir::resp
