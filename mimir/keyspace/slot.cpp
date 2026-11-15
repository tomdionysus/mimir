//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/slot.h"

#include <array>

namespace mimir::keyspace {

namespace {

// CRC16 XMODEM: polynomial 0x1021, initial value 0, no reflection.
constexpr std::array<uint16_t, 256> make_table() {
  std::array<uint16_t, 256> table{};
  for (unsigned i = 0; i < 256; ++i) {
    uint16_t crc = static_cast<uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint16_t, 256> table = make_table();

}  // namespace

uint16_t crc16(std::string_view bytes) noexcept {
  uint16_t crc = 0;
  for (unsigned char c : bytes) crc = static_cast<uint16_t>((crc << 8) ^ table[((crc >> 8) ^ c) & 0xff]);
  return crc;
}

uint16_t key_slot(std::string_view key) noexcept {
  const size_t open = key.find('{');
  if (open != std::string_view::npos) {
    const size_t close = key.find('}', open + 1);
    if (close != std::string_view::npos && close > open + 1) key = key.substr(open + 1, close - open - 1);
  }
  return static_cast<uint16_t>(crc16(key) & (slot_count - 1));
}

}  // namespace mimir::keyspace
