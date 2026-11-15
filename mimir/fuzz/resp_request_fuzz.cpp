//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The RESP request parser over arbitrary bytes, fed in arbitrary pieces: it
// returns commands or refuses, never anything else, and holds no more than it
// was given.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "mimir/resp/parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  mimir::resp::ParserLimits limits;
  limits.max_arguments = 64;
  limits.max_bulk = 4096;
  limits.max_inline = 1024;
  mimir::resp::RequestParser parser(limits);
  parser.restrict((data[0] & 1) != 0);
  // The first byte also sets the piece size, so pieces split every boundary.
  const size_t piece = static_cast<size_t>(data[0] >> 1) + 1;
  std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
  size_t fed = 0;
  try {
    while (fed < bytes.size()) {
      const auto part = bytes.substr(fed, piece);
      parser.feed(part);
      fed += part.size();
      while (auto command = parser.next()) {
        if (command->empty() || command->size() > limits.max_arguments) std::abort();
      }
      if (parser.buffered() > fed) std::abort();
    }
  } catch (const mimir::resp::ProtocolError&) {
  }
  return 0;
}
