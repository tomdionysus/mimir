//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The reply side of RESP, in either protocol a connection has chosen with
// HELLO. A command writes its reply in RESP3's terms (maps, sets, doubles,
// booleans, nulls) and the writer encodes it as the connection speaks: RESP2
// flattens a map to an array of its keys and values, a set to an array, a
// double to a bulk string, a boolean to an integer, and has two nulls.
namespace mimir::resp {

enum class Protocol : uint8_t { resp2 = 2, resp3 = 3 };

// A double as Valkey writes it: the shortest text that reads back as the same
// value; "inf", "-inf" and "nan" for those.
std::string format_double(double value);

// Appends one reply's bytes to a buffer. Aggregates are written as a header
// with their count, then each element by further calls. Not thread-safe.
class Reply {
 public:
  explicit Reply(Protocol protocol = Protocol::resp2) : _protocol(protocol) {}

  Protocol protocol() const noexcept { return _protocol; }

  void simple(std::string_view text);
  // `text` whole, its code first ("ERR syntax error", "WRONGTYPE ..."). A line
  // end inside it becomes a space: an error is one line.
  void error(std::string_view text);
  void integer(int64_t value);
  void bulk(std::string_view bytes);
  // RESP2 "$-1"; RESP3 "_".
  void null();
  // RESP2 "*-1"; RESP3 "_".
  void null_array();
  void array(size_t count);
  // `pairs` keys each followed by its value. RESP2: an array of 2 x pairs.
  void map(size_t pairs);
  // RESP2: an array.
  void set(size_t count);
  // An out-of-band message (pub/sub). RESP2: an array.
  void push(size_t count);
  // RESP2: a bulk string.
  void real(double value);
  // RESP2: 1 or 0.
  void boolean(bool value);
  // RESP3 "(": the digits as given. RESP2: a bulk string.
  void big_number(std::string_view digits);
  // RESP3 "=": `format` is three characters ("txt"). RESP2: a bulk string.
  void verbatim(std::string_view format, std::string_view text);

  // Bytes already encoded as a reply (one queued command's, inside EXEC's).
  void raw(std::string_view bytes) { _bytes.append(bytes); }

  const std::string& bytes() const noexcept { return _bytes; }
  std::string take() noexcept { return std::move(_bytes); }
  void clear() noexcept { _bytes.clear(); }

 private:
  void header(char type, int64_t count);

  Protocol _protocol;
  std::string _bytes;
};

}  // namespace mimir::resp
