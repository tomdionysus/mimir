//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/hll.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace mimir::keyspace::hll {

namespace {

constexpr size_t index_bits = 14;
// Register values run 0 to 64 - 14 + 1.
constexpr size_t max_value = 64 - index_bits + 1;
constexpr uint8_t sparse_max_value = 32;

uint64_t load64(const unsigned char* p) noexcept {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

uint8_t dense_get(std::string_view body, size_t r) noexcept {
  const size_t bit = r * 6;
  const size_t byte = bit / 8;
  const unsigned shift = bit & 7;
  unsigned value = static_cast<unsigned char>(body[byte]) >> shift;
  if (byte + 1 < body.size()) value |= static_cast<unsigned>(static_cast<unsigned char>(body[byte + 1])) << (8 - shift);
  return static_cast<uint8_t>(value & 63);
}

void dense_set(std::string& out, size_t offset, size_t r, uint8_t value) noexcept {
  const size_t bit = r * 6;
  const size_t byte = offset + bit / 8;
  const unsigned shift = bit & 7;
  auto& low = reinterpret_cast<unsigned char&>(out[byte]);
  low = static_cast<unsigned char>((low & ~(63u << shift)) | (static_cast<unsigned>(value) << shift));
  if (shift > 2 && byte + 1 < out.size()) {
    auto& high = reinterpret_cast<unsigned char&>(out[byte + 1]);
    high = static_cast<unsigned char>((high & ~(63u >> (8 - shift))) | (static_cast<unsigned>(value) >> (8 - shift)));
  }
}

// Ertl's sigma and tau, as the improved estimator defines them.
long double sigma(long double x) {
  if (x == 1.0L) return std::numeric_limits<long double>::infinity();
  long double y = 1.0L;
  long double z = x;
  long double previous;
  do {
    x *= x;
    previous = z;
    z += x * y;
    y += y;
  } while (previous != z);
  return z;
}

long double tau(long double x) {
  if (x == 0.0L || x == 1.0L) return 0.0L;
  long double y = 1.0L;
  long double z = 1.0L - x;
  long double previous;
  do {
    x = std::sqrt(x);
    previous = z;
    y *= 0.5L;
    z -= (1.0L - x) * (1.0L - x) * y;
  } while (previous != z);
  return z / 3.0L;
}

}  // namespace

uint64_t murmur64a(std::string_view bytes, uint64_t seed) noexcept {
  const uint64_t m = 0xc6a4a7935bd1e995ull;
  const int r = 47;
  const size_t len = bytes.size();
  uint64_t h = seed ^ (len * m);
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const size_t blocks = len / 8;
  for (size_t i = 0; i < blocks; ++i) {
    uint64_t k = load64(data + i * 8);
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }
  const unsigned char* tail = data + blocks * 8;
  switch (len & 7) {
    case 7:
      h ^= static_cast<uint64_t>(tail[6]) << 48;
      [[fallthrough]];
    case 6:
      h ^= static_cast<uint64_t>(tail[5]) << 40;
      [[fallthrough]];
    case 5:
      h ^= static_cast<uint64_t>(tail[4]) << 32;
      [[fallthrough]];
    case 4:
      h ^= static_cast<uint64_t>(tail[3]) << 24;
      [[fallthrough]];
    case 3:
      h ^= static_cast<uint64_t>(tail[2]) << 16;
      [[fallthrough]];
    case 2:
      h ^= static_cast<uint64_t>(tail[1]) << 8;
      [[fallthrough]];
    case 1:
      h ^= static_cast<uint64_t>(tail[0]);
      h *= m;
  }
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

std::pair<size_t, uint8_t> position(std::string_view element) noexcept {
  uint64_t hash = murmur64a(element);
  const size_t index = static_cast<size_t>(hash & (registers - 1));
  hash >>= index_bits;
  // A sentinel bit bounds the run of zeros.
  hash |= 1ull << (64 - index_bits);
  uint8_t count = 1;
  for (uint64_t bit = 1; (hash & bit) == 0; bit <<= 1) ++count;
  return {index, count};
}

std::string make_empty() {
  Registers zero{};
  std::string out = encode(zero, false);
  // A new value's cache is valid: its count is zero.
  store_cache(out, 0);
  return out;
}

bool looks_valid(std::string_view bytes) noexcept {
  if (bytes.size() < header_size || std::memcmp(bytes.data(), "HYLL", 4) != 0) return false;
  const auto encoding = static_cast<unsigned char>(bytes[4]);
  if (encoding > 1) return false;
  if (encoding == 0 && bytes.size() != dense_size) return false;
  return true;
}

bool is_dense(std::string_view bytes) noexcept { return bytes.size() > 4 && bytes[4] == 0; }

std::optional<Registers> decode(std::string_view bytes) {
  Registers out{};
  std::string_view body = bytes.substr(header_size);
  if (is_dense(bytes)) {
    for (size_t r = 0; r < registers; ++r) out[r] = dense_get(body, r);
    return out;
  }
  size_t r = 0;
  for (size_t i = 0; i < body.size();) {
    const auto op = static_cast<unsigned char>(body[i]);
    size_t run;
    uint8_t value = 0;
    if ((op & 0xc0) == 0) {
      run = (op & 0x3f) + 1u;
      i += 1;
    } else if ((op & 0xc0) == 0x40) {
      if (i + 1 >= body.size()) return std::nullopt;
      run = ((static_cast<size_t>(op & 0x3f) << 8) | static_cast<unsigned char>(body[i + 1])) + 1;
      i += 2;
    } else {
      value = static_cast<uint8_t>(((op >> 2) & 0x1f) + 1);
      run = (op & 0x3) + 1u;
      i += 1;
    }
    if (r + run > registers) return std::nullopt;
    for (size_t k = 0; k < run; ++k) out[r + k] = value;
    r += run;
  }
  if (r != registers) return std::nullopt;
  return out;
}

std::string encode(const Registers& regs, bool dense) {
  std::string header("HYLL", 4);
  header.append(12, '\0');
  bool fits = !dense;
  if (fits) {
    for (uint8_t v : regs) {
      if (v > sparse_max_value) {
        fits = false;
        break;
      }
    }
  }
  if (fits) {
    std::string out = header;
    out[4] = 1;
    size_t r = 0;
    while (r < registers) {
      const uint8_t value = regs[r];
      size_t run = 1;
      while (r + run < registers && regs[r + run] == value) ++run;
      size_t left = run;
      if (value == 0) {
        while (left > 0) {
          if (left <= 64) {
            out.push_back(static_cast<char>(left - 1));
            left = 0;
          } else {
            const size_t take = std::min<size_t>(left, 16384);
            out.push_back(static_cast<char>(0x40 | ((take - 1) >> 8)));
            out.push_back(static_cast<char>((take - 1) & 0xff));
            left -= take;
          }
        }
      } else {
        while (left > 0) {
          const size_t take = std::min<size_t>(left, 4);
          out.push_back(static_cast<char>(0x80 | ((value - 1) << 2) | (take - 1)));
          left -= take;
        }
      }
      r += run;
    }
    if (out.size() <= sparse_max_bytes) {
      out[15] = static_cast<char>(0x80);
      return out;
    }
  }
  std::string out = header;
  out.resize(dense_size, '\0');
  out[4] = 0;
  for (size_t r = 0; r < registers; ++r) dense_set(out, header_size, r, regs[r]);
  out[15] = static_cast<char>(0x80);
  return out;
}

uint64_t estimate(const Registers& regs) {
  const long double m = static_cast<long double>(registers);
  std::array<size_t, max_value + 2> histogram{};
  for (uint8_t v : regs) ++histogram[v];
  const size_t q = 64 - index_bits;
  long double z = m * tau((m - static_cast<long double>(histogram[q + 1])) / m);
  for (size_t j = q; j >= 1; --j) {
    z += static_cast<long double>(histogram[j]);
    z *= 0.5L;
  }
  z += m * sigma(static_cast<long double>(histogram[0]) / m);
  const long double alpha = 0.721347520444481703680L;
  return static_cast<uint64_t>(std::llroundl(alpha * m * m / z));
}

std::optional<uint64_t> cached(std::string_view bytes) noexcept {
  if (bytes.size() < header_size || (static_cast<unsigned char>(bytes[15]) & 0x80)) return std::nullopt;
  return load64(reinterpret_cast<const unsigned char*>(bytes.data()) + 8);
}

void store_cache(std::string& bytes, uint64_t cardinality) noexcept {
  for (int i = 0; i < 8; ++i) bytes[8 + static_cast<size_t>(i)] = static_cast<char>((cardinality >> (8 * i)) & 0xff);
}

}  // namespace mimir::keyspace::hll
