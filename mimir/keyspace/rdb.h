//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mimir/keyspace/keyspace.h"
#include "mimir/keyspace/limits.h"

// DUMP's and RESTORE's serialisation: one value in the RDB encoding Valkey 8
// writes (RDB version 11), so a payload moves between Mimir and Valkey either
// way. A payload is the value's type and body, then the version (two bytes,
// little-endian), then a CRC-64 (Jones) of everything before it.
//
// Mimir writes each value in the encoding Valkey would hold it in (OBJECT
// ENCODING's answer): a listpack for a small list, hash, set or sorted set,
// an intset for a small set of integers, LZF for a long compressible string.
// It reads every encoding Valkey 8 reads, the older ziplist ones included.
// What it reads is untrusted: every length is checked against what remains,
// and what does not parse is refused, never trusted.
namespace mimir::keyspace::rdb {

inline constexpr uint16_t version = 11;

// CRC-64 with Jones's coefficients, reflected, as Valkey checks a payload.
uint64_t crc64(uint64_t crc, std::string_view bytes) noexcept;

// LZF as Valkey's RDB writes it: the compressed bytes, or none when they would
// not fit in `limit` bytes.
std::optional<std::string> lzf_compress(std::string_view in, size_t limit);
// The `length` bytes `in` expands to, or none when it is not LZF that does.
std::optional<std::string> lzf_decompress(std::string_view in, size_t length);

// The bytes one element takes in a listpack as Valkey writes it (an element
// that is an integer's text is held as the integer), and an integer's.
size_t listpack_element_size(std::string_view element);
size_t listpack_integer_size(int64_t value);

// DUMP's payload for `entry`'s value (its expiry is RESTORE's argument).
std::string dump(const Entry& entry);

// Why a payload was refused: RESTORE's two errors.
enum class Refusal { checksum, format };

// RESTORE: the value a payload holds, with no expiry, or why not. Without
// `load`, each value keeps the encoding it was dumped in (the cluster's own
// records); with it, a client's RESTORE, each is loaded as Valkey loads it,
// in the encoding those limits give its size.
std::variant<Entry, Refusal> restore(std::string_view payload, const Limits* load = nullptr);

// FUNCTION DUMP's payload: each library's code; and FUNCTION RESTORE's
// reading of one.
std::string dump_functions(const std::vector<std::string>& codes);
std::variant<std::vector<std::string>, Refusal> restore_functions(std::string_view payload);

}  // namespace mimir::keyspace::rdb
