//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The RESP reply reader (MIGRATE's, reading another server) over arbitrary
// bytes, fed in arbitrary pieces: it returns replies or refuses, never
// anything else, holds no more than it was given, and keeps no text longer
// than its bounds.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "mimir/resp/parser.h"
#include "mimir/resp/reply_reader.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  mimir::resp::ReplyLimits limits;
  limits.max_line = 256;
  limits.max_bulk = 1024;
  limits.max_elements = 64;
  limits.max_depth = 4;
  mimir::resp::ReplyReader reader(limits);
  const size_t piece = static_cast<size_t>(data[0]) + 1;
  std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
  size_t fed = 0;
  try {
    while (fed < bytes.size()) {
      const auto part = bytes.substr(fed, piece);
      reader.feed(part);
      fed += part.size();
      while (auto reply = reader.next()) {
        if (reply->text.size() > limits.max_bulk && reply->text.size() > limits.max_line) std::abort();
      }
      if (reader.buffered() > fed) std::abort();
    }
  } catch (const mimir::resp::ProtocolError&) {
  }
  return 0;
}
