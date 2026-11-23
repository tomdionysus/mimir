//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// HyperLogLog in the representation Valkey and Redis share, so a value moves
// between them by DUMP and RESTORE or GET and SET: a string beginning "HYLL",
// an encoding byte (0 dense, 1 sparse), three unused bytes and an eight-byte
// little-endian cached cardinality whose top bit marks it stale, then 16,384
// six-bit registers, packed (dense) or run-length coded (sparse). An element
// is hashed with MurmurHash64A (seed 0xadc83b19); its low 14 bits pick the
// register and the run of zeros above them, plus one, is the value kept if
// larger. The count is Ertl's improved raw estimator.
//
// Mimir writes the sparse form canonically (each run as one opcode, or as
// few as the opcodes allow), which any reader of the format accepts; Valkey's
// incremental updates can leave a longer opcode sequence, so the bytes of an
// updated sparse value may differ from Valkey's while every register, and so
// every count, is the same.
namespace mimir::keyspace::hll {

inline constexpr size_t registers = 16384;
inline constexpr size_t header_size = 16;
inline constexpr size_t dense_size = header_size + (registers * 6 + 7) / 8;
// Valkey's hll-sparse-max-bytes: a sparse value longer than this is made dense.
inline constexpr size_t sparse_max_bytes = 3000;

using Registers = std::array<uint8_t, registers>;

uint64_t murmur64a(std::string_view bytes, uint64_t seed = 0xadc83b19ull) noexcept;
// An element's register and value.
std::pair<size_t, uint8_t> position(std::string_view element) noexcept;

// An empty HyperLogLog, sparse.
std::string make_empty();
// Whether `bytes` is a HyperLogLog by its header and length (WRONGTYPE otherwise).
bool looks_valid(std::string_view bytes) noexcept;
bool is_dense(std::string_view bytes) noexcept;
// The registers; none when the body is corrupt (INVALIDOBJ).
std::optional<Registers> decode(std::string_view bytes);
// `registers` encoded sparse when it fits in `sparse_max` bytes (the agreed
// hll-sparse-max-bytes) and no value exceeds 32, unless `dense`; the cached
// cardinality marked stale.
std::string encode(const Registers& registers, bool dense, size_t sparse_max = sparse_max_bytes);
// The estimate over `registers`.
uint64_t estimate(const Registers& registers);
// The cached cardinality, when valid.
std::optional<uint64_t> cached(std::string_view bytes) noexcept;
void store_cache(std::string& bytes, uint64_t cardinality) noexcept;

}  // namespace mimir::keyspace::hll
