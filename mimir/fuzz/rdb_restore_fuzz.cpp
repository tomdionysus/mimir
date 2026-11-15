//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// RESTORE's reader over arbitrary bytes: it returns a value or refuses, never
// anything else, and a value it returns dumps and reads back as itself. The
// checksum is made to hold for half the inputs, so the fuzzer reaches the
// value's body rather than stopping at the footer.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>

#include "mimir/keyspace/rdb.h"

namespace rdb = mimir::keyspace::rdb;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  std::string payload(reinterpret_cast<const char*>(data) + 1, size - 1);
  if (data[0] & 1) {
    // A valid footer over the body.
    payload.push_back(static_cast<char>(rdb::version & 0xff));
    payload.push_back(static_cast<char>(rdb::version >> 8));
    const uint64_t crc = rdb::crc64(0, payload);
    for (int i = 0; i < 8; ++i) payload.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  }
  auto restored = rdb::restore(payload);
  if (auto* entry = std::get_if<mimir::keyspace::Entry>(&restored)) {
    const std::string again = rdb::dump(*entry);
    auto back = rdb::restore(again);
    if (!std::holds_alternative<mimir::keyspace::Entry>(back)) std::abort();
    if (rdb::dump(std::get<mimir::keyspace::Entry>(back)) != again) std::abort();
  }
  (void)rdb::restore_functions(payload);
  return 0;
}
