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
#include <utility>
#include <vector>

// The GEO commands' arithmetic, as Valkey stores and searches positions: a
// position is a sorted set member's score, the 52-bit interleave of its
// latitude (even bits) and longitude (odd bits), each scaled over Valkey's
// range (longitude -180 to 180, latitude -85.05112878 to 85.05112878, the
// Web Mercator limits) into 26 bits. A search covers a geohash box and its
// eight neighbours at a step estimated from the radius, then measures each
// member there exactly, so its unsorted order is Valkey's too.
namespace mimir::keyspace::geo {

inline constexpr double longitude_min = -180;
inline constexpr double longitude_max = 180;
inline constexpr double latitude_min = -85.05112878;
inline constexpr double latitude_max = 85.05112878;
inline constexpr unsigned full_step = 26;

struct Point {
  double longitude;
  double latitude;
};

struct Bits {
  uint64_t bits = 0;
  unsigned step = 0;
  bool zero() const noexcept { return bits == 0 && step == 0; }
};

// The 52-bit score of a position in range, or none outside it.
std::optional<uint64_t> encode(Point p);
// The centre of the cell a score names, kept within range.
Point decode(uint64_t score);
// GEOHASH's 11 characters for a score: the position re-encoded over the
// standard latitude range (-90 to 90), the last character always '0'.
std::string geohash_text(uint64_t score);
// Metres between two positions on Valkey's sphere.
double distance(Point a, Point b);

// What a search covers.
struct Shape {
  Point centre;
  bool box = false;
  // In metres: a radius, or a box's width and height.
  double radius = 0;
  double width = 0;
  double height = 0;
};

// The metres from the centre to `p` when it lies in the shape, else none.
std::optional<double> within(const Shape& shape, Point p);

// The cells a search reads, in Valkey's order (the centre's, then north,
// south, east, west, north-east, north-west, south-east, south-west), each
// as a [min, max) range of scores; a cell excluded is left out, and one the
// same as the cell read before it (a very large radius) is skipped.
std::vector<std::pair<uint64_t, uint64_t>> search_cells(const Shape& shape);

}  // namespace mimir::keyspace::geo
