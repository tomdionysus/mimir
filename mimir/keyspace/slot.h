//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string_view>

// Where a key lives: Valkey's cluster hash slots, so a cluster-aware client
// computes the same slot Mimir does. A key's slot is CRC16 (XMODEM) of the key,
// or of its hash tag, modulo 16384. The hash tag is what lies between the first
// '{' and the first '}' after it, when that is not empty, so keys that share a
// tag share a slot and a shard.
namespace mimir::keyspace {

inline constexpr uint16_t slot_count = 16384;

uint16_t crc16(std::string_view bytes) noexcept;
uint16_t key_slot(std::string_view key) noexcept;

}  // namespace mimir::keyspace
